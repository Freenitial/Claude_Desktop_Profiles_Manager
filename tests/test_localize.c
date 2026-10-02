/* What the program relies on even without tools/check-localization.py: keys
 * sorted for the binary search, printf contracts, no empty text; language
 * selection; resource labels, their reading direction and forgetting them;
 * names cut to fit; the Delete key's name in menus, as Windows' keyboard
 * layouts give it. Catalog wording is the Python checker's. */
#include "../src/app.h"
#include <stdio.h>
#include <wchar.h>

/* Columns of the catalog (localize.c's kLanguages). */
#define ENGLISH_LANGUAGE    0
#define FRENCH_LANGUAGE     1
#define GERMAN_LANGUAGE     2
#define SPANISH_LANGUAGE    3
#define ARABIC_LANGUAGE     4
#define CHINESE_LANGUAGE    5
#define HINDI_LANGUAGE      7
#define PORTUGUESE_LANGUAGE 8
#define BENGALI_LANGUAGE    9
#define URDU_LANGUAGE       11
#define LANGUAGE_COUNT      12
#define LANGUAGE_VALUE      L"Language"   /* localize.c's value under REG_ROOT */

static int g_checks, g_failures;

static BOOL Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
    return ok;
}

static BOOL Has(const WCHAR *set, WCHAR c) { return c && wcschr(set, c) != NULL; }

/* The complete token includes width, precision and length: %lu and %s
 * cannot be exchanged, and a '*' width is an additional argument. */
static const WCHAR *FormatToken(const WCHAR *cursor, WCHAR *token, size_t cch)
{
    while (*cursor) {
        const WCHAR *start, *p;
        if (*cursor++ != L'%') continue;
        start = cursor - 1;
        p = cursor;
        if (*p == L'%') p++;
        else {
            while (Has(L"-+ #0", *p)) p++;
            if (*p == L'*') p++;
            else while (*p >= L'0' && *p <= L'9') p++;
            if (*p == L'.') {
                p++;
                if (*p == L'*') p++;
                else while (*p >= L'0' && *p <= L'9') p++;
            }
            if (p[0] == L'I' && p[1] == L'6' && p[2] == L'4') p += 3;
            else if (p[0] == L'l' && p[1] == L'l') p += 2;
            else if (Has(L"lhzw", *p)) p++;
            if (!Has(L"diuoxXfFeEgGaAcCsSpn", *p)) continue;
            p++;
        }
        StringCchCopyNW(token, cch, start, (size_t)(p - start));
        return p;
    }
    token[0] = 0;
    return cursor;
}

static BOOL SameFormats(const WCHAR *a, const WCHAR *b)
{
    WCHAR x[64], y[64];
    do {
        a = FormatToken(a, x, ARRAYSIZE(x));
        b = FormatToken(b, y, ARRAYSIZE(y));
        if (wcscmp(x, y) != 0) return FALSE;
    } while (x[0]);
    return TRUE;
}

static void Catalog(void)
{
    size_t i;
    int language;
    Check("twelve interface languages", Localize_LanguageCount() == LANGUAGE_COUNT);
    for (i = 0; i < Localize_CatalogCount(); i++) {
        const WCHAR *key = Localize_CatalogKey(i);
        WCHAR copy[2048];
        if (!Check("nonempty key", key && *key)) continue;
        Check("keys sorted and unique for the binary search", !i || wcscmp(Localize_CatalogKey(i - 1), key) < 0);
        StringCchCopyW(copy, ARRAYSIZE(copy), key);
        Check("the lookup finds every key by its text", Localize_TranslateAt(0, copy) == key);
        for (language = 0; language < Localize_LanguageCount(); language++) {
            const WCHAR *text = Localize_TranslateAt(language, key);
            if (!Check("nonempty translation in every language", text && *text)) continue;
            if (!Check("exact printf contract", SameFormats(key, text)))
                wprintf(L"    language=%s key=[%s] translation=[%s]\n", Localize_LanguageCode(language), key, text);
        }
    }
    Check("out of bounds catalog key", Localize_CatalogKey(Localize_CatalogCount()) == NULL);
    Check("fallback preserves unknown user text",
          wcscmp(Localize_TranslateAt(FRENCH_LANGUAGE, L"My own session title"), L"My own session title") == 0);
    Check("null lookup", Localize_Text(NULL) == NULL);
    Check("invalid language falls back to English", wcscmp(Localize_TranslateAt(999, L"Close"), L"Close") == 0);
    Check("32 bit unsigned formats differ", !SameFormats(L"%lu", L"%s"));
    Check("width argument differs", !SameFormats(L"%.*s", L"%s"));
    Check("argument order differs", !SameFormats(L"%d %s", L"%s %d"));
    Check("literal percent differs", !SameFormats(L"%%", L"%s"));
}

/* The saved language, or "" when none is. */
static void SavedLanguage(WCHAR *code, size_t cch)
{
    if (!Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, LANGUAGE_VALUE, code, cch)) code[0] = 0;
}

static void Languages(void)
{
    WCHAR savedBefore[32], savedAfter[32];
    int language;
    Check("French OS language", Localize_LanguageForCode(L"fr-FR") == FRENCH_LANGUAGE);
    Check("German OS language", Localize_LanguageForCode(L"de-DE") == GERMAN_LANGUAGE);
    Check("Arabic OS language", Localize_LanguageForCode(L"ar-SA") == ARABIC_LANGUAGE);
    Check("Chinese OS language", Localize_LanguageForCode(L"zh-Hans-CN") == CHINESE_LANGUAGE);
    Check("every Chinese locale uses the simplified catalog",
          Localize_LanguageForCode(L"zh-TW") == CHINESE_LANGUAGE && Localize_LanguageForCode(L"zh") == CHINESE_LANGUAGE);
    Check("the Portuguese catalog is Brazil's", wcscmp(Localize_LanguageCode(PORTUGUESE_LANGUAGE), L"pt-BR") == 0);
    Check("every Portuguese locale, and a code saved as pt, uses it",
          Localize_LanguageForCode(L"pt-BR") == PORTUGUESE_LANGUAGE && Localize_LanguageForCode(L"pt-PT") == PORTUGUESE_LANGUAGE &&
          Localize_LanguageForCode(L"pt") == PORTUGUESE_LANGUAGE);
    Check("unknown language", Localize_LanguageForCode(L"xx-XX") == -1);
    Check("language prefix is not enough", Localize_LanguageForCode(L"french") == -1);
    Check("empty locale", Localize_LanguageForCode(L"") == -1);
    SavedLanguage(savedBefore, ARRAYSIZE(savedBefore));
    for (language = 0; language < Localize_LanguageCount(); language++) {
        BOOL rtl = language == ARABIC_LANGUAGE || language == URDU_LANGUAGE;
        Check("language can be selected for this run only", Localize_SetLanguage(language, FALSE));
        Check("selection and effective language agree", Localize_CurrentLanguage() == language && Localize_EffectiveLanguage() == language);
        Check("language code round trips", Localize_LanguageForCode(Localize_LanguageCode(language)) == language);
        Check("RTL only for Arabic and Urdu", Localize_IsRTL() == rtl);
        Check("RTL draw flag", Localize_ReadingFlags() == (UINT)(rtl ? DT_RTLREADING : 0));
        Check("lookup observes selected language", Localize_Text(L"Close") == Localize_TranslateAt(language, L"Close"));
    }
    SavedLanguage(savedAfter, ARRAYSIZE(savedAfter));
    Check("a selection for this run only leaves the saved language", wcscmp(savedBefore, savedAfter) == 0);
    Check("invalid selection rejected", !Localize_SetLanguage(LANGUAGE_COUNT, FALSE));
    Check("invalid negative selection rejected", !Localize_SetLanguage(-2, FALSE));
    Check("invalid indexed fonts fall back to English",
          wcscmp(Localize_FontFaceAt(-1), Localize_FontFaceAt(ENGLISH_LANGUAGE)) == 0 &&
          wcscmp(Localize_FontFaceAt(LANGUAGE_COUNT), Localize_FontFaceAt(ENGLISH_LANGUAGE)) == 0);
    Check("invalid indexed directions fall back to English", !Localize_IsRTLAt(-1) && !Localize_IsRTLAt(LANGUAGE_COUNT));
    Check("Hindi script font", Localize_SetLanguage(HINDI_LANGUAGE, FALSE) && wcscmp(Localize_FontFace(), L"Nirmala UI") == 0);
    Check("Bengali script font", Localize_SetLanguage(BENGALI_LANGUAGE, FALSE) && wcscmp(Localize_FontFace(), L"Nirmala UI") == 0);
    Check("Chinese script font",
          Localize_SetLanguage(CHINESE_LANGUAGE, FALSE) && wcscmp(Localize_FontFace(), L"Microsoft YaHei UI") == 0);
    Check("automatic language", Localize_SetLanguage(-1, FALSE) && Localize_CurrentLanguage() == -1);
}

static BOOL ReadsRightToLeft(HWND window)
{
    return (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_RTLREADING) != 0;
}

static void ResourceLabels(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    HWND dialog = CreateWindowExW(0, L"Static", L"Edit profile", WS_POPUP, 0, 0, 300, 200, NULL, NULL, instance, NULL);
    HWND button = CreateWindowExW(0, L"Button", L"Cancel", WS_CHILD, 0, 0, 90, 24, dialog, NULL, instance, NULL);
    HWND edit = CreateWindowExW(0, L"Edit", L"Cancel", WS_CHILD | ES_AUTOHSCROLL, 0, 30, 150, 24, dialog, NULL, instance, NULL);
    HWND data = CreateWindowExW(0, L"Static", L"", WS_CHILD, 0, 70, 150, 24, dialog, NULL, instance, NULL);
    WCHAR text[128];
    if (!Check("hidden resource label fixture", dialog && button && edit && data)) {
        if (dialog) DestroyWindow(dialog);
        return;
    }
    Localize_SetLanguage(FRENCH_LANGUAGE, FALSE);
    Localize_Window(dialog);
    GetWindowTextW(button, text, ARRAYSIZE(text));
    Check("resource button translated", wcscmp(text, Localize_Text(L"Cancel")) == 0);
    GetWindowTextW(edit, text, ARRAYSIZE(text));
    Check("editable user title never translated", wcscmp(text, L"Cancel") == 0);
    SetWindowTextW(data, L"Cancel");
    Localize_SetLanguage(GERMAN_LANGUAGE, FALSE);
    Localize_Window(dialog);
    GetWindowTextW(button, text, ARRAYSIZE(text));
    Check("captured English key supports live switch", wcscmp(text, Localize_Text(L"Cancel")) == 0);
    GetWindowTextW(data, text, ARRAYSIZE(text));
    Check("dynamic user text never captured on switch", wcscmp(text, L"Cancel") == 0);
    Localize_SetLanguage(ARABIC_LANGUAGE, FALSE);
    Localize_Window(dialog);
    Check("the dialog reads right to left", ReadsRightToLeft(dialog));
    Check("translated button reads right to left", ReadsRightToLeft(button));
    Check("label the program fills reads right to left", ReadsRightToLeft(data));
    Check("editable data direction preserved", (GetWindowLongPtrW(edit, GWL_EXSTYLE) & (WS_EX_RTLREADING | WS_EX_LAYOUTRTL)) == 0);
    Localize_SetLanguage(FRENCH_LANGUAGE, FALSE);
    Localize_Window(dialog);
    Check("back from Arabic, labels read left to right", !ReadsRightToLeft(button) && !ReadsRightToLeft(data) && !ReadsRightToLeft(dialog));
    Localize_Window(NULL);
    Localize_ForgetWindow(NULL);
    /* Forgotten, a label's text is read again as it is: user text there stays. */
    Localize_ForgetWindow(dialog);
    SetWindowTextW(button, L"My own title");
    Localize_Window(dialog);
    GetWindowTextW(button, text, ARRAYSIZE(text));
    Check("a forgotten label no longer shows its old key", wcscmp(text, L"My own title") == 0);
    Localize_ForgetWindow(dialog);
    DestroyWindow(dialog);
    Localize_SetLanguage(ENGLISH_LANGUAGE, FALSE);
}

static void CutNames(void)
{
    static const WCHAR kPair[] = L"A\xD83D\xDE00Z";   /* a surrogate pair between two letters */
    WCHAR text[256], longName[LABEL_CCH + 8];
    size_t i;
    Localize_FormatCutName(L"[%s]", L"Work", 4, text, ARRAYSIZE(text));
    Check("a whole name has no ellipsis", wcscmp(text, L"[Work]") == 0);
    Localize_FormatCutName(L"[%s]", L"Work", 2, text, ARRAYSIZE(text));
    Check("a cut name ends with an ellipsis", wcscmp(text, L"[Wo\x2026]") == 0);
    Localize_FormatCutCaption(L"&Copy [%s]", L"R&D team", 3, text, ARRAYSIZE(text));
    Check("a caption doubles the name's ampersands after the cut", wcscmp(text, L"&Copy [R&&D\x2026]") == 0);
    Localize_FormatCutName(L"[%s]", L"R&D", 3, text, ARRAYSIZE(text));
    Check("a label keeps the name's ampersands", wcscmp(text, L"[R&D]") == 0);
    for (i = 0; i < ARRAYSIZE(longName) - 1; i++) longName[i] = L'a';
    longName[i] = 0;
    Localize_FormatCutName(L"%s", longName, ARRAYSIZE(longName) - 1, text, ARRAYSIZE(text));
    Check("a name longer than a display name is cut, with its ellipsis",
          wcslen(text) == LABEL_CCH && text[LABEL_CCH - 1] == L'\x2026');
    Check("a cut is one character shorter", Localize_ShorterCut(L"Work", 4) == 3);
    Check("a cut never splits a surrogate pair", Localize_ShorterCut(kPair, 3) == 1);
    Check("a cut stops at nothing", Localize_ShorterCut(L"W", 1) == 0 && Localize_ShorterCut(L"", 0) == 0);
    Check("a cut keeps a letter with its combining mark", Localize_ShorterCut(L"Ae\x0301", 3) == 1);
    Check("a cut keeps a Devanagari vowel sign with its letter", Localize_ShorterCut(L"\x0915\x093F", 2) == 0);
    Check("a cut keeps a Devanagari conjunct and its vowel sign whole (pro)",
          Localize_ShorterCut(L"\x092A\x094D\x0930\x094B", 4) == 0);
    Check("a cut keeps a conjunct whole (ksha)", Localize_ShorterCut(L"\x0915\x094D\x0937", 3) == 0);
    Check("a cut keeps a Bengali conjunct whole (pra)", Localize_ShorterCut(L"\x09AA\x09CD\x09B0", 3) == 0);
    Check("a cut keeps a visarga with its letter", Localize_ShorterCut(L"\x0915\x0903", 2) == 0);
    Check("a cut keeps a Bengali anusvara with its letter", Localize_ShorterCut(L"\x09AC\x0982", 2) == 0);
    Check("a cut still parts two letters", Localize_ShorterCut(L"\x0905\x092C", 2) == 1 &&
          Localize_ShorterCut(L"\x09A8\x09BE\x09AE", 3) == 2);
    Check("a cut keeps an emoji with its variation selector", Localize_ShorterCut(L"A\x2764\xFE0F", 3) == 1);
    Check("a cut keeps an emoji with its skin tone", Localize_ShorterCut(L"A\xD83D\xDC4D\xD83C\xDFFD", 5) == 1);
    Check("a cut keeps a joined emoji sequence whole",
          Localize_ShorterCut(L"A\xD83D\xDC69\x200D\xD83D\xDCBB", 6) == 1);
    Check("a cut keeps a flag's two letters together",
          Localize_ShorterCut(L"\xD83C\xDDEB\xD83C\xDDF7\xD83C\xDDE9\xD83C\xDDEA", 8) == 4 &&
          Localize_ShorterCut(L"\xD83C\xDDEB\xD83C\xDDF7\xD83C\xDDE9\xD83C\xDDEA", 4) == 0);
}

/* The start of a keyboard layout's tables (KBDTABLES of the WDK's kbd.h):
 * its key names, the extended keys' (E0) apart. */
typedef struct KeyName {
    BYTE scanCode;
    const WCHAR *name;
} KeyName;

typedef struct KeyboardTables {
    const void *modifiers, *characters, *deadKeys;
    const KeyName *names, *extendedNames;
} KeyboardTables;

typedef const KeyboardTables *(WINAPI *KeyboardDescriptor)(void);

/* The Delete key's name (E0 53) in Windows' keyboard layout `file`, in
 * `name`; FALSE when the layout is not there. */
static BOOL DeleteKeyName(const WCHAR *file, WCHAR *name, size_t cch)
{
    HMODULE layout = LoadLibraryExW(file, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    KeyboardDescriptor descriptor = layout ? (KeyboardDescriptor)(void *)GetProcAddress(layout, "KbdLayerDescriptor") : NULL;
    const KeyboardTables *tables = descriptor ? descriptor() : NULL;
    const KeyName *key;
    BOOL found = FALSE;
    name[0] = 0;
    for (key = tables ? tables->extendedNames : NULL; key && key->scanCode && !found; key++)
        if (key->scanCode == 0x53) found = SUCCEEDED(StringCchCopyW(name, cch, key->name));
    if (layout) FreeLibrary(layout);
    return found;
}

/* The menus' name of the Delete key (the text after a tab) is the one the
 * language's keyboard layout gives it, in capitals there; where it has none
 * of its own, it is English's "Del", as Windows' menus write "Delete". */
static void KeyNames(void)
{
    static const struct { int language; const WCHAR *layout; } kLayouts[] = {
        { FRENCH_LANGUAGE, L"kbdfr.dll" }, { GERMAN_LANGUAGE, L"kbdgr.dll" }, { SPANISH_LANGUAGE, L"kbdsp.dll" }
    };
    WCHAR name[64];
    char label[160];
    size_t i;
    if (!DeleteKeyName(L"kbdus.dll", name, ARRAYSIZE(name))) {
        printf("  skip  key names: the US keyboard layout cannot be read\n");
        return;
    }
    Check("the US keyboard layout names the Delete key Delete (its tables are read right)", wcscmp(name, L"Delete") == 0);
    for (i = 0; i < ARRAYSIZE(kLayouts); i++) {
        const WCHAR *shown = Localize_TranslateAt(kLayouts[i].language, L"\tDel") + 1;
        StringCchPrintfA(label, ARRAYSIZE(label), "the Delete key is named as %ls names it", kLayouts[i].layout);
        if (!DeleteKeyName(kLayouts[i].layout, name, ARRAYSIZE(name))) {
            printf("  skip  %s: the layout cannot be read\n", label);
        } else if (!Check(label, CompareStringOrdinal(name, -1, shown, -1, TRUE) == CSTR_EQUAL)) {
            wprintf(L"        layout %s, catalog %s\n", name, shown);
        }
    }
}

int wmain(void)
{
    Catalog();
    Languages();
    ResourceLabels();
    CutNames();
    KeyNames();
    printf("Localization tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
