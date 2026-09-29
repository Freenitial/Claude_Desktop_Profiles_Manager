/*
 * Pure helpers: no registry, no files, no UI. Everything here is covered by
 * tests/test_core.c.
 */
#include "app.h"
#include <math.h>
#include <string.h>
#include <wchar.h>

/* ---------------------------------------------------------------- strings */

static BOOL IsSpace(WCHAR c)
{
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x00A0 || c == 0x3000;
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

static BOOL EqualsI(const WCHAR *a, int la, const WCHAR *b, int lb)
{
    return CompareStringOrdinal(a, la, b, lb, TRUE) == CSTR_EQUAL;
}

BOOL Core_EndsWithI(const WCHAR *s, const WCHAR *suffix)
{
    size_t ls, lx;
    if (!s || !suffix) return FALSE;
    ls = wcslen(s);
    lx = wcslen(suffix);
    return lx <= ls && EqualsI(s + ls - lx, (int)lx, suffix, (int)lx);
}

static const WCHAR *FindI(const WCHAR *s, const WCHAR *needle)
{
    size_t ls = wcslen(s), ln = wcslen(needle), i;
    if (ln == 0 || ln > ls) return NULL;
    for (i = 0; i + ln <= ls; i++)
        if (EqualsI(s + i, (int)ln, needle, (int)ln)) return s + i;
    return NULL;
}

BOOL Core_ContainsI(const WCHAR *s, const WCHAR *needle)
{
    return s && needle && FindI(s, needle) != NULL;
}

static size_t LengthWithoutTrailingSlash(const WCHAR *p)
{
    size_t n = wcslen(p);
    while (n > 3 && (p[n - 1] == L'\\' || p[n - 1] == L'/')) n--;
    return n;
}

BOOL Core_PathEquals(const WCHAR *a, const WCHAR *b)
{
    size_t la, lb;
    if (!a || !b || !*a || !*b) return FALSE;
    la = LengthWithoutTrailingSlash(a);
    lb = LengthWithoutTrailingSlash(b);
    return EqualsI(a, (int)la, b, (int)lb);
}

BOOL Core_IsOurExe(const WCHAR *path)
{
    const WCHAR *name;
    if (!path || !*path) return FALSE;
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return EqualsI(name, -1, APP_EXE, -1);
}

DWORD Core_Hash(const WCHAR *s)
{
    WCHAR upper[512];
    DWORD h = 2166136261u;
    int n, i;
    n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s, -1, upper, ARRAYSIZE(upper), NULL, NULL, 0);
    if (n <= 0) return h;
    for (i = 0; i < n - 1; i++) {
        h ^= (DWORD)upper[i];
        h *= 16777619u;
    }
    return h;
}

void Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, APP_AUMID_PREFIX L"Profile.%08lX", (unsigned long)Core_Hash(folder));
}

ULONGLONG Core_SystemTimeTicks(const SYSTEMTIME *st)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    if (!SystemTimeToFileTime(st, &ft)) return 0;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

/* ----------------------------------------------------------- profile names */

static BOOL IsNameChar(WCHAR c)
{
    WORD type = 0;
    if (c == L' ' || c == L'-' || c == L'_' || c == L'.') return TRUE;
    if (c < 0x20 || (c >= 0xD800 && c <= 0xDFFF)) return FALSE;
    if (!GetStringTypeW(CT_CTYPE1, &c, 1, &type)) return FALSE;
    return (type & (C1_ALPHA | C1_DIGIT)) != 0;
}

/* Claude keeps its own siblings next to a profile folder: "<folder>-Data" in
 * %LOCALAPPDATA% and "Claude-3p" in %APPDATA%. A profile named like one of
 * them would share those directories with another profile. */
static BOOL IsReservedSuffix(const WCHAR *s, size_t n)
{
    static const WCHAR *const exact[] = { L"3p", L"Data" };
    static const WCHAR *const tails[] = { L"-3p", L"-Data" };
    size_t i, lt;
    for (i = 0; i < ARRAYSIZE(exact); i++)
        if (EqualsI(s, (int)n, exact[i], -1)) return TRUE;
    for (i = 0; i < ARRAYSIZE(tails); i++) {
        lt = wcslen(tails[i]);
        if (n >= lt && EqualsI(s + n - lt, (int)lt, tails[i], (int)lt)) return TRUE;
    }
    return FALSE;
}

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
            *error = L"Use letters, digits, spaces, '-', '_' or '.'.";
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
        if (start[i] < 0x20) { *error = L"The name contains an invalid character."; return FALSE; }
    }
    return SUCCEEDED(StringCchCopyNW(label, cch, start, n));
}

BOOL Core_IsProfileFolder(const WCHAR *folder)
{
    size_t lp = wcslen(PROFILE_PREFIX), n, i;
    if (!folder) return FALSE;
    n = wcslen(folder);
    if (n <= lp || n >= FOLDER_CCH) return FALSE;
    if (!EqualsI(folder, (int)lp, PROFILE_PREFIX, (int)lp)) return FALSE;
    for (i = lp; i < n; i++)
        if (folder[i] < 0x20) return FALSE;
    return !IsReservedSuffix(folder + lp, n - lp);
}

/* ------------------------------------------------------------------- links */

BOOL Core_SanitizeUrl(const WCHAR *in, WCHAR *out, size_t cch)
{
    static const WCHAR hex[] = L"0123456789ABCDEF";
    const WCHAR *start;
    size_t n, i, o = 0;
    if (!in || !out || cch == 0) return FALSE;
    out[0] = 0;
    n = TrimmedSpan(in, &start);
    if (n < 8 || !EqualsI(start, 7, L"claude:", 7)) return FALSE;
    for (i = 0; i < n; i++) {
        WCHAR c = start[i];
        /* The link travels as one quoted argument: a quote, a backslash (it
         * can escape the closing quote) or whitespace must not reach argv. */
        if (c < 0x20 || c == 0x7F || c == L'"' || c == L'\\' || c == L' ') {
            if (o + 3 >= cch) return FALSE;
            out[o++] = L'%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        } else if (IsSpace(c)) {
            return FALSE;
        } else {
            if (o + 1 >= cch) return FALSE;
            out[o++] = c;
        }
    }
    out[o] = 0;
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
        size_t n = LengthWithoutTrailingSlash(dataDir);
        if (wcschr(dataDir, L'"')) return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"--user-data-dir=\"%.*s\"", (int)n, dataDir))) return FALSE;
    }
    if (url && *url) {
        if (wcschr(url, L'"') || wcschr(url, L'\\')) return FALSE;
        if (out[0] && FAILED(StringCchCatW(out, cch, L" "))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, L"\""))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, url))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, L"\""))) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------ log parsing */

static BOOL IsDigit(char c) { return c >= '0' && c <= '9'; }

static BOOL IsTimestamp(const char *p)
{
    static const char shape[] = "dddd-dd-dd dd:dd:dd";
    int i;
    for (i = 0; i < 19; i++) {
        if (shape[i] == 'd' ? !IsDigit(p[i]) : p[i] != shape[i]) return FALSE;
    }
    return TRUE;
}

static int Num(const char *p, int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++) v = v * 10 + (p[i] - '0');
    return v;
}

static BOOL LineContains(const char *p, const char *end, const char *needle)
{
    size_t ln = strlen(needle);
    for (; (size_t)(end - p) >= ln; p++)
        if (memcmp(p, needle, ln) == 0) return TRUE;
    return FALSE;
}

static BOOL StampToTime(const char *stamp, SYSTEMTIME *st)
{
    ZeroMemory(st, sizeof *st);
    st->wYear = (WORD)Num(stamp, 4);
    st->wMonth = (WORD)Num(stamp + 5, 2);
    st->wDay = (WORD)Num(stamp + 8, 2);
    st->wHour = (WORD)Num(stamp + 11, 2);
    st->wMinute = (WORD)Num(stamp + 14, 2);
    st->wSecond = (WORD)Num(stamp + 17, 2);
    return st->wMonth >= 1 && st->wMonth <= 12 && st->wDay >= 1 && st->wDay <= 31 &&
           st->wHour < 24 && st->wMinute < 60 && st->wSecond < 60;
}

/* Claude logs "<yyyy-mm-dd hh:mm:ss> [info] [Auth] Using system browser for:
 * /login/..." in the main.log of the window that opened the browser. */
BOOL Core_LatestSignInStart(const char *text, size_t len, SYSTEMTIME *latest)
{
    const char *p = text, *end = text + len;
    char best[19];
    BOOL found = FALSE;
    if (!text || !latest) return FALSE;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        if (le - p > 19 && IsTimestamp(p) && LineContains(p + 19, le, "[Auth] Using system browser for:")) {
            if (!found || memcmp(p, best, 19) > 0) {
                memcpy(best, p, 19);
                found = TRUE;
            }
        }
        p = nl ? nl + 1 : end;
    }
    return found && StampToTime(best, latest);
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
    const char *p = text, *end = text + len, *last = NULL;
    BOOL lastUpdate = FALSE;
    size_t i;
    if (!text || !when || !forUpdate) return FALSE;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *le = nl ? nl : end;
        if (le - p > 19 && IsTimestamp(p)) {
            BOOL isUpdate = FALSE, isOther = FALSE;
            for (i = 0; i < ARRAYSIZE(update) && !isUpdate; i++) isUpdate = LineContains(p + 19, le, update[i]);
            for (i = 0; i < ARRAYSIZE(other) && !isUpdate && !isOther; i++) isOther = LineContains(p + 19, le, other[i]);
            if (isUpdate || isOther) {
                last = p;
                lastUpdate = isUpdate;
            }
        }
        p = nl ? nl + 1 : end;
    }
    if (!last || !StampToTime(last, when)) return FALSE;
    *forUpdate = lastUpdate;
    return TRUE;
}

/* --------------------------------------------------------------- routing */

int Core_SelectTargets(int count, const ULONGLONG *signInTicks, ULONGLONG nowTicks,
                       ULONGLONG windowTicks, BOOL signInUrl, int lastUsed,
                       int defaultIdx, int *targets, RouteReason *reason)
{
    const ULONGLONG skew = 60ULL * 10000000ULL;
    RouteReason r;
    int n = 0, i, best = -1;

    if (count <= 0) {
        r = ROUTE_NOTHING_RUNNING;
    } else if (count == 1) {
        targets[n++] = 0;
        r = ROUTE_ONLY_ONE;
    } else if (signInUrl) {
        for (i = 0; i < count; i++) {
            ULONGLONG t = signInTicks ? signInTicks[i] : 0;
            if (!t || t > nowTicks + skew) continue;
            if (nowTicks > t && nowTicks - t > windowTicks) continue;
            if (best < 0 || t > signInTicks[best]) best = i;
        }
        if (best >= 0) {
            targets[n++] = best;
            r = ROUTE_SIGNIN;
        } else {
            for (i = 0; i < count; i++) targets[n++] = i;
            r = ROUTE_BROADCAST;
        }
    } else if (lastUsed >= 0 && lastUsed < count) {
        targets[n++] = lastUsed;
        r = ROUTE_LAST_USED;
    } else if (defaultIdx >= 0 && defaultIdx < count) {
        targets[n++] = defaultIdx;
        r = ROUTE_DEFAULT;
    } else {
        targets[n++] = 0;
        r = ROUTE_FIRST;
    }
    if (reason) *reason = r;
    return n;
}

const WCHAR *Core_RouteReasonText(RouteReason reason)
{
    switch (reason) {
    case ROUTE_NOTHING_RUNNING: return L"no Claude window open: default profile";
    case ROUTE_ONLY_ONE:        return L"only open window";
    case ROUTE_SIGNIN:          return L"window that started the sign-in";
    case ROUTE_BROADCAST:       return L"sign-in link claimed by no window: sent to every window";
    case ROUTE_LAST_USED:       return L"last used window";
    case ROUTE_DEFAULT:         return L"default profile";
    default:                    return L"first open window";
    }
}

/* ---------------------------------------------------------------- shortcuts */

static const WCHAR *NextToken(const WCHAR *p, WCHAR *tok, size_t cch)
{
    size_t o = 0;
    BOOL quoted = FALSE;
    while (*p && IsSpace(*p)) p++;
    if (!*p) return NULL;
    while (*p && (quoted || !IsSpace(*p))) {
        if (*p == L'"') { quoted = !quoted; p++; continue; }
        if (o + 1 < cch) tok[o++] = *p;
        p++;
    }
    tok[o] = 0;
    return p;
}

BOOL Core_ArgsSelectProfile(const WCHAR *args, const WCHAR *folder)
{
    WCHAR tok[256];
    BOOL next = FALSE;
    const WCHAR *p = args;
    if (!args || !folder || !*folder) return FALSE;
    while ((p = NextToken(p, tok, ARRAYSIZE(tok))) != NULL) {
        if (next) return EqualsI(tok, -1, folder, -1);
        next = EqualsI(tok, -1, L"--launch", -1);
    }
    return FALSE;
}

BOOL Core_ArgsReferenceDir(const WCHAR *args, const WCHAR *dir)
{
    size_t dl;
    const WCHAR *p;
    WCHAR bare[MAX_PATH];
    if (!args || !dir || !*dir) return FALSE;
    dl = LengthWithoutTrailingSlash(dir);
    if (dl < 4 || FAILED(StringCchCopyNW(bare, ARRAYSIZE(bare), dir, dl))) return FALSE;
    for (p = FindI(args, bare); p; p = FindI(p + 1, bare)) {
        WCHAR before = p == args ? L' ' : p[-1];
        WCHAR after = p[dl];
        BOOL okBefore = before == L'"' || before == L'=' || IsSpace(before);
        BOOL okAfter = after == 0 || after == L'"' || IsSpace(after) ||
                       (after == L'\\' && (p[dl + 1] == 0 || p[dl + 1] == L'"' || IsSpace(p[dl + 1])));
        if (okBefore && okAfter) return TRUE;
    }
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
        if (!link->target[0] && Core_EndsWithI(link->parsing, L"!Claude") &&
            (Core_ContainsI(link->parsing, L"\\Claude_") || Core_ContainsI(link->parsing, L".Claude_")))
            return TRUE;
    }
    return FALSE;
}

/* `path` is `dir` itself or inside it (case-insensitive, whole folder names). */
BOOL Core_PathUnder(const WCHAR *path, const WCHAR *dir)
{
    size_t dl;
    if (!path || !dir) return FALSE;
    dl = LengthWithoutTrailingSlash(dir);
    if (dl == 0 || wcslen(path) < dl || !EqualsI(path, (int)dl, dir, (int)dl)) return FALSE;
    return path[dl] == 0 || path[dl] == L'\\';
}

/* The two times are the same to the shell (FAT date and time, 2-second steps). */
BOOL Core_SameFatTime(const FILETIME *a, const FILETIME *b)
{
    WORD da, ta, db, tb;
    if (!FileTimeToDosDateTime(a, &da, &ta) || !FileTimeToDosDateTime(b, &db, &tb)) return FALSE;
    return da == db && ta == tb;
}

void Core_ShortcutFileName(const WCHAR *label, int copy, WCHAR *out, size_t cch)
{
    WCHAR clean[LABEL_CCH];
    size_t i;
    if (FAILED(StringCchCopyW(clean, ARRAYSIZE(clean), label ? label : L""))) clean[0] = 0;
    for (i = 0; clean[i]; i++) {
        if (clean[i] < 0x20 || wcschr(L"\\/:*?\"<>|", clean[i])) clean[i] = L'_';
    }
    if (copy > 1)
        StringCchPrintfW(out, cch, L"Claude (%s) (%d).lnk", clean, copy);
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

/* The raw text of member `key` of the JSON object `json` (not of a nested
 * one), as it is written: quotes, braces and all. */
BOOL Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen)
{
    size_t i = 0, keyStart, keyEnd, end, keyLen = strlen(key);
    if (len >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF) i = 3;
    i = SkipSpace(json, len, i);
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

static void PutUtf8(char *out, size_t *o, DWORD cp)
{
    if (cp < 0x80) {
        out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        out[(*o)++] = (char)(0xC0 | (cp >> 6));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*o)++] = (char)(0xE0 | (cp >> 12));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*o)++] = (char)(0xF0 | (cp >> 18));
        out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*o)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL Hex4(const char *p, DWORD *v)
{
    int i, d;
    *v = 0;
    for (i = 0; i < 4; i++) {
        if ((d = HexDigit(p[i])) < 0) return FALSE;
        *v = (*v << 4) | (DWORD)d;
    }
    return TRUE;
}

/* A JSON string as Core_JsonMember gives it (quotes included), decoded to
 * UTF-16. FALSE when it is not a string, is malformed or does not fit. */
BOOL Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch)
{
    char *utf8;
    size_t i, o = 0;
    int n;
    if (!raw || !out || cch == 0 || len < 2 || raw[0] != '"' || raw[len - 1] != '"') return FALSE;
    out[0] = 0;
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, len);
    if (!utf8) return FALSE;
    for (i = 1; i + 1 < len; i++) {
        char c = raw[i];
        DWORD cp, low;
        if (c != '\\') {
            utf8[o++] = c;
            continue;
        }
        if (++i + 1 >= len) break;
        switch (raw[i]) {
        case '"': case '\\': case '/': utf8[o++] = raw[i]; break;
        case 'b': utf8[o++] = '\b'; break;
        case 'f': utf8[o++] = '\f'; break;
        case 'n': utf8[o++] = '\n'; break;
        case 'r': utf8[o++] = '\r'; break;
        case 't': utf8[o++] = '\t'; break;
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
            PutUtf8(utf8, &o, cp);
            break;
        default:
            goto bad;
        }
    }
    if (o == 0) {
        HeapFree(GetProcessHeap(), 0, utf8);
        return TRUE;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)o, out, (int)cch - 1);
    HeapFree(GetProcessHeap(), 0, utf8);
    if (n <= 0) {
        out[0] = 0;
        return FALSE;
    }
    out[n] = 0;
    return TRUE;
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
        if (!IsDigit(raw[i]) || *value > (~0ULL - 9) / 10) return FALSE;
        *value = *value * 10 + (ULONGLONG)(raw[i] - '0');
    }
    return TRUE;
}

BOOL Core_JsonTrue(const char *raw, size_t len)
{
    return raw && len == 4 && memcmp(raw, "true", 4) == 0;
}

/* The member is there and not null. */
static BOOL JsonSet(const char *json, size_t len, const char *key)
{
    const char *v;
    size_t n;
    return Core_JsonMember(json, len, key, &v, &n) && !(n == 4 && memcmp(v, "null", 4) == 0);
}

/* What a Code session entry (claude-code-sessions\...\local_*.json) is: a
 * session of this PC, one run over SSH, in WSL or in the cloud (its
 * conversation is elsewhere), or not an entry (no sessionId). */
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len)
{
    const char *v;
    size_t n;
    if (!json || !Core_JsonMember(json, len, "sessionId", &v, &n) || n < 3 || v[0] != '"') return ENTRY_NOT_ONE;
    if (JsonSet(json, len, "sshConfig") || JsonSet(json, len, "wslConfig") || JsonSet(json, len, "cloudSessionId") ||
        (Core_JsonMember(json, len, "movedToCloud", &v, &n) && Core_JsonTrue(v, n)))
        return ENTRY_ELSEWHERE;
    return ENTRY_LOCAL;
}

/* ---------------------------------------------------------- session edits */

/* `json` with its top-level member `key` set to `raw` (a JSON value as
 * text): replaced where it is, else added last in the object, the rest kept
 * byte for byte. FALSE when `json` is not an object or `out` is too small. */
BOOL Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen)
{
    const char *v;
    size_t n, rawLen = strlen(raw), keyLen = strlen(key), head, tail, last, need, start;
    BOOL empty;
    *outLen = 0;
    start = len >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF ? 3 : 0;
    start = SkipSpace(json, len, start);
    if (start >= len || json[start] != '{') return FALSE;
    if (Core_JsonMember(json, len, key, &v, &n)) {
        head = (size_t)(v - json);
        tail = len - head - n;
        if (head + rawLen + tail > cap) return FALSE;
        memcpy(out, json, head);
        memcpy(out + head, raw, rawLen);
        memcpy(out + head + rawLen, v + n, tail);
        *outLen = head + rawLen + tail;
        return TRUE;
    }
    /* Not there (or not an object): added after the last value. */
    for (last = len; last > 0 && IsJsonSpace(json[last - 1]); last--) {}
    if (last == 0 || json[last - 1] != '}') return FALSE;
    for (head = last - 1; head > start && IsJsonSpace(json[head - 1]); head--) {}
    empty = head == start + 1;
    need =head + (empty ? 0 : 1) + 1 + keyLen + 2 + rawLen + (len - head);
    if (need > cap) return FALSE;
    memcpy(out, json, head);
    n = head;
    if (!empty) out[n++] = ',';
    out[n++] = '"';
    memcpy(out + n, key, keyLen);
    n += keyLen;
    out[n++] = '"';
    out[n++] = ':';
    memcpy(out + n, raw, rawLen);
    n += rawLen;
    memcpy(out + n, json + head, len - head);
    *outLen = n + (len - head);
    return TRUE;
}

/* `s` as a JSON string, quotes included, in UTF-8. */
BOOL Core_JsonQuote(const WCHAR *s, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    const WCHAR *p;
#define PUT(c) do { if (n + 1 >= cap) return FALSE; ((unsigned char *)out)[n++] = (unsigned char)(c); } while (0)
    PUT('"');
    for (p = s; *p; p++) {
        unsigned int c = *p;
        if (c == '"' || c == '\\') {
            PUT('\\');
            PUT(c);
        } else if (c < 0x20) {
            PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hex[c >> 4]); PUT(hex[c & 15]);
        } else if (c < 0x80) {
            PUT(c);
        } else if (c < 0x800) {
            PUT(0xC0 | (c >> 6));
            PUT(0x80 | (c & 0x3F));
        } else if (c >= 0xD800 && c <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
            unsigned int cp = 0x10000 + ((c - 0xD800) << 10) + (p[1] - 0xDC00);
            p++;
            PUT(0xF0 | (cp >> 18));
            PUT(0x80 | ((cp >> 12) & 0x3F));
            PUT(0x80 | ((cp >> 6) & 0x3F));
            PUT(0x80 | (cp & 0x3F));
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            PUT(0xEF); PUT(0xBF); PUT(0xBD);   /* a lone surrogate: U+FFFD */
        } else {
            PUT(0xE0 | (c >> 12));
            PUT(0x80 | ((c >> 6) & 0x3F));
            PUT(0x80 | (c & 0x3F));
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

/* A Claude Code session id: 8-4-4-4-12 hex digits. */
BOOL Core_IsSessionId(const WCHAR *id)
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
    return Core_IsSessionId(sessionId) && SUCCEEDED(StringCchPrintfW(out, cch, L"claude://resume?session=%s", sessionId));
}

/* A new "no folder" working folder's name, as Claude Desktop names them:
 * scratch-<date>-<6 hex digits>. */
void Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, L"scratch-%04u-%02u-%02u-%06lx", day->wYear, day->wMonth, day->wDay, (unsigned long)(random & 0xFFFFFF));
}

/* Copies `in` to `out` with every `swaps[i].from` replaced by its `to`,
 * for a file read in chunks: when a swap could start in the last bytes and
 * this is not the `last` chunk, they are left for the next one (`*used` says
 * how much of `in` was taken). Returns the bytes written; `out` holds at
 * least len + (len / shortest from + 1) * longest to bytes. */
size_t Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used)
{
    size_t i = 0, n = 0;
    int s;
    while (i < len) {
        BOOL held = FALSE, swapped = FALSE;
        for (s = 0; s < count; s++) {
            size_t fromLen = strlen(swaps[s].from), left = len - i;
            if (fromLen == 0) continue;
            if (left >= fromLen) {
                if (memcmp(in + i, swaps[s].from, fromLen) == 0) {
                    size_t toLen = strlen(swaps[s].to);
                    memcpy(out + n, swaps[s].to, toLen);
                    n += toLen;
                    i += fromLen;
                    swapped = TRUE;
                    break;
                }
            } else if (!last && memcmp(in + i, swaps[s].from, left) == 0) {
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

BOOL Core_PendingFormat(const PendingEdit *e, WCHAR *out, size_t cch)
{
    WCHAR value[SESSION_TITLE_CCH];
    size_t i;
    if ((int)e->op < 0 || (int)e->op >= (int)ARRAYSIZE(kPendingOps) || !e->key[0] || wcspbrk(e->key, L"\t\r\n")) return FALSE;
    StringCchCopyW(value, ARRAYSIZE(value), e->value);
    for (i = 0; value[i]; i++)
        if (value[i] == L'\t' || value[i] == L'\r' || value[i] == L'\n') value[i] = L' ';
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\t%s\t%s", kPendingOps[e->op], e->key, value));
}

BOOL Core_PendingParse(const WCHAR *line, PendingEdit *e)
{
    const WCHAR *tab1 = wcschr(line, L'\t'), *tab2;
    size_t op;
    ZeroMemory(e, sizeof *e);
    if (!tab1 || (tab2 = wcschr(tab1 + 1, L'\t')) == NULL || tab2 == tab1 + 1) return FALSE;
    for (op = 0; op < ARRAYSIZE(kPendingOps); op++)
        if ((size_t)(tab1 - line) == wcslen(kPendingOps[op]) && wcsncmp(line, kPendingOps[op], (size_t)(tab1 - line)) == 0) break;
    if (op == ARRAYSIZE(kPendingOps)) return FALSE;
    e->op = (PendingOp)op;
    if (FAILED(StringCchCopyNW(e->key, ARRAYSIZE(e->key), tab1 + 1, (size_t)(tab2 - tab1 - 1)))) return FALSE;
    StringCchCopyW(e->value, ARRAYSIZE(e->value), tab2 + 1);
    return TRUE;
}

/* ------------------------------------------------------------ drawing math */

/* One frame of a smooth wheel scroll: how many px of the `pending` ones it
 * covers, `elapsedMs` after the last frame. What is left shrinks by e every
 * CORE_SCROLL_EASE_MS, whatever the frame rate (a late frame covers more, so
 * the pace holds), and by at least a pixel, so it ends; never past it. */
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

/* "v1.2.3" or "1.2" as up to four numbers; FALSE when it does not start with one. */
BOOL Core_ParseVersion(const WCHAR *text, DWORD parts[4])
{
    int n = 0;
    ZeroMemory(parts, 4 * sizeof(DWORD));
    if (!text) return FALSE;
    if (*text == L'v' || *text == L'V') text++;
    while (n < 4 && *text >= L'0' && *text <= L'9') {
        DWORD v = 0;
        while (*text >= L'0' && *text <= L'9') {
            v = v * 10 + (DWORD)(*text - L'0');
            if (v > 65535) return FALSE;
            text++;
        }
        parts[n++] = v;
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
