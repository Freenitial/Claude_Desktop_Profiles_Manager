/* Interface translations are immutable; only the selected language changes. */
#include "app.h"
#include <wchar.h>

typedef struct Language {
    const WCHAR *code;
    const WCHAR *name;
    const WCHAR *font;
    BOOL rtl;
} Language;

/* In the order of the catalog's columns. */
static const Language kLanguages[] = {
    { L"en", L"English", L"Segoe UI", FALSE },
    { L"fr", L"Fran\x00E7" L"ais", L"Segoe UI", FALSE },
    { L"de", L"Deutsch", L"Segoe UI", FALSE },
    { L"es", L"Espa\x00F1ol", L"Segoe UI", FALSE },
    { L"ar", L"\x0627\x0644\x0639\x0631\x0628\x064A\x0629", L"Segoe UI", TRUE },
    { L"zh-CN", L"\x7B80\x4F53\x4E2D\x6587", L"Microsoft YaHei UI", FALSE },
    { L"ru", L"\x0420\x0443\x0441\x0441\x043A\x0438\x0439", L"Segoe UI", FALSE },
    { L"hi", L"\x0939\x093F\x0928\x094D\x0926\x0940", L"Nirmala UI", FALSE },
    { L"pt-BR", L"Portugu\x00EAs (Brasil)", L"Segoe UI", FALSE },
    { L"bn", L"\x09AC\x09BE\x0982\x09B2\x09BE", L"Nirmala UI", FALSE },
    { L"id", L"Bahasa Indonesia", L"Segoe UI", FALSE },
    { L"ur", L"\x0627\x0631\x062F\x0648", L"Segoe UI", TRUE }
};

typedef struct Translation {
    const WCHAR *text[ARRAYSIZE(kLanguages)];
} Translation;

static const Translation kCatalog[] = {
#include "localize_catalog.inc"
};

#define LANGUAGE_VALUE L"Language"   /* under REG_ROOT: the chosen language's code, absent to follow Windows */
/* On a resource label: its English key, or kNoKey when its text is not one
 * (user data, never translated). */
#define LABEL_PROP L"ClaudeDesktopProfilesManager.LocalizeKey"

static const WCHAR kNoKey[] = L"";
static volatile LONG g_requested = -1;
static volatile LONG g_effective = 0;

int Localize_LanguageCount(void) { return (int)ARRAYSIZE(kLanguages); }
int Localize_CurrentLanguage(void) { return (int)InterlockedCompareExchange(&g_requested, 0, 0); }
int Localize_EffectiveLanguage(void) { return (int)InterlockedCompareExchange(&g_effective, 0, 0); }

const WCHAR *Localize_LanguageCode(int language)
{
    return language >= 0 && language < Localize_LanguageCount() ? kLanguages[language].code : L"auto";
}

const WCHAR *Localize_LanguageName(int language)
{
    return language >= 0 && language < Localize_LanguageCount() ? kLanguages[language].name : TR(L"System language");
}

/* A locale matches a language by its language subtag, so every Chinese
 * locale uses the simplified catalog and every Portuguese one the Brazilian
 * catalog (named as such: European Windows calls some things otherwise). A
 * code saved as "pt" still matches. */
int Localize_LanguageForCode(const WCHAR *code)
{
    int i;
    if (!code || !code[0]) return -1;
    for (i = 0; i < Localize_LanguageCount(); i++) {
        size_t length = wcscspn(kLanguages[i].code, L"-");
        if (_wcsnicmp(code, kLanguages[i].code, length) == 0 && (!code[length] || code[length] == L'-')) return i;
    }
    return -1;
}

static int WindowsLanguage(void)
{
    WCHAR names[1024];
    ULONG count = 0, chars = ARRAYSIZE(names);
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, names, &chars)) {
        const WCHAR *p;
        for (p = names; *p; p += wcslen(p) + 1) {
            int language = Localize_LanguageForCode(p);
            if (language >= 0) return language;
        }
    }
    return 0;
}

BOOL Localize_SetLanguage(int language, BOOL persist)
{
    if (language < -1 || language >= Localize_LanguageCount()) return FALSE;
    if (persist) {
        if (language < 0) {
            LSTATUS status = Util_RegDeleteValue(HKEY_CURRENT_USER, REG_ROOT, LANGUAGE_VALUE);
            if (status != ERROR_SUCCESS) {
                Util_Log(L"could not forget the interface language (error %ld)", status);
                return FALSE;
            }
        } else if (!Util_RegSetString(HKEY_CURRENT_USER, REG_ROOT, LANGUAGE_VALUE, Localize_LanguageCode(language))) {
            return FALSE;
        }
    }
    InterlockedExchange(&g_requested, language);
    InterlockedExchange(&g_effective, language < 0 ? WindowsLanguage() : language);
    return TRUE;
}

void Localize_Init(void)
{
    WCHAR code[32];
    Localize_SetLanguage(Util_RegGetString(HKEY_CURRENT_USER, REG_ROOT, LANGUAGE_VALUE, code, ARRAYSIZE(code))
                         ? Localize_LanguageForCode(code) : -1, FALSE);
}

static const Translation *FindTranslation(const WCHAR *english)
{
    size_t first = 0, last = ARRAYSIZE(kCatalog);
    if (!english) return NULL;
    while (first < last) {
        size_t middle = first + (last - first) / 2;
        int order = wcscmp(english, kCatalog[middle].text[0]);
        if (order == 0) return &kCatalog[middle];
        if (order < 0) last = middle;
        else first = middle + 1;
    }
    return NULL;
}

const WCHAR *Localize_TranslateAt(int language, const WCHAR *english)
{
    const Translation *entry = FindTranslation(english);
    if (language < 0 || language >= Localize_LanguageCount()) language = 0;
    return entry ? entry->text[language] : english;
}

const WCHAR *Localize_Text(const WCHAR *english)
{
    return Localize_TranslateAt(Localize_EffectiveLanguage(), english);
}

const WCHAR *Localize_FontFaceAt(int language)
{
    if (language < 0 || language >= Localize_LanguageCount()) language = 0;
    return kLanguages[language].font;
}

BOOL Localize_IsRTLAt(int language)
{
    if (language < 0 || language >= Localize_LanguageCount()) language = 0;
    return kLanguages[language].rtl;
}

const WCHAR *Localize_FontFace(void) { return Localize_FontFaceAt(Localize_EffectiveLanguage()); }
BOOL Localize_IsRTL(void) { return Localize_IsRTLAt(Localize_EffectiveLanguage()); }
UINT Localize_ReadingFlags(void) { return Localize_IsRTL() ? DT_RTLREADING : 0; }
size_t Localize_CatalogCount(void) { return ARRAYSIZE(kCatalog); }
const WCHAR *Localize_CatalogKey(size_t index) { return index < ARRAYSIZE(kCatalog) ? kCatalog[index].text[0] : NULL; }

/* The first time, the window's text tells whether it is a key; after that
 * its key is translated again, whatever text it shows. Every label reads in
 * the language's direction, the ones the program fills included. */
static void TranslateLabel(HWND window)
{
    const WCHAR *key = (const WCHAR *)GetPropW(window, LABEL_PROP);
    LONG_PTR style = GetWindowLongPtrW(window, GWL_EXSTYLE), readingStyle;
    if (!key) {
        WCHAR text[2048];
        const Translation *entry;
        GetWindowTextW(window, text, ARRAYSIZE(text));
        entry = text[0] ? FindTranslation(text) : NULL;
        key = entry ? entry->text[0] : kNoKey;
        SetPropW(window, LABEL_PROP, (HANDLE)key);
    }
    if (key != kNoKey) SetWindowTextW(window, TR(key));
    readingStyle = Localize_IsRTL() ? style | WS_EX_RTLREADING : style & ~(LONG_PTR)WS_EX_RTLREADING;
    if (readingStyle != style) {
        SetWindowLongPtrW(window, GWL_EXSTYLE, readingStyle);
        InvalidateRect(window, NULL, TRUE);
    }
}

static BOOL CALLBACK TranslateChild(HWND child, LPARAM param)
{
    WCHAR className[32];
    (void)param;
    GetClassNameW(child, className, ARRAYSIZE(className));
    if (_wcsicmp(className, L"Button") == 0 || _wcsicmp(className, L"Static") == 0) TranslateLabel(child);
    return TRUE;
}

/* A NULL window would make EnumChildWindows walk every top-level window. */
void Localize_Window(HWND dialog)
{
    if (!dialog) return;
    TranslateLabel(dialog);
    EnumChildWindows(dialog, TranslateChild, 0);
}

static BOOL CALLBACK ForgetChild(HWND child, LPARAM param)
{
    (void)param;
    RemovePropW(child, LABEL_PROP);
    return TRUE;
}

void Localize_ForgetWindow(HWND dialog)
{
    if (!dialog) return;
    EnumChildWindows(dialog, ForgetChild, 0);
    RemovePropW(dialog, LABEL_PROP);
}

/* A flag is two regional indicator letters (U+1F1E6..U+1F1FF). */
static BOOL IsRegionalIndicatorAt(const WCHAR *text, size_t index)
{
    return text[index] == 0xD83C && text[index + 1] >= 0xDDE6 && text[index + 1] <= 0xDDFF;
}

/* The Brahmic blocks from Devanagari (U+0900) to Malayalam (U+0D7F) share one
 * layout of 0x80 characters: signs at 0x00-0x03 (anusvara, visarga), the
 * nukta at 0x3C, vowel signs and the virama at 0x3E-0x4D, length and stress
 * marks at 0x51-0x57, vowel signs at 0x62-0x63; Devanagari adds vowel signs at
 * 0x3A-0x3B and 0x4E-0x4F, Bengali its sandhi mark. Windows calls some of
 * these neither nonspacing nor vowel marks (visarga, Bengali anusvara). */
#define BRAHMIC_FIRST 0x0900
#define BRAHMIC_LAST  0x0D7F
#define BRAHMIC_BLOCK 0x80
#define BRAHMIC_VIRAMA_OFFSET 0x4D

static BOOL IsBrahmicMark(WCHAR c)
{
    unsigned offset;
    if (c < BRAHMIC_FIRST || c > BRAHMIC_LAST) return FALSE;
    offset = ((unsigned)c - BRAHMIC_FIRST) % BRAHMIC_BLOCK;
    return offset <= 0x03 || offset == 0x3C || (offset >= 0x3E && offset <= 0x4D) || (offset >= 0x51 && offset <= 0x57) ||
           offset == 0x62 || offset == 0x63 || (c >= 0x093A && c <= 0x093B) || (c >= 0x094E && c <= 0x094F) || c == 0x09FE;
}

/* A consonant after a virama is the second half of a conjunct. */
static BOOL IsVirama(WCHAR c)
{
    return c >= BRAHMIC_FIRST && c <= BRAHMIC_LAST && ((unsigned)c - BRAHMIC_FIRST) % BRAHMIC_BLOCK == BRAHMIC_VIRAMA_OFFSET;
}

/* The character at `index` belongs with the one before it on screen: the
 * second half of a surrogate pair, a combining mark or vowel sign, what
 * follows a virama, a variation selector, the keycap mark, what a zero-width
 * joiner joins, an emoji skin tone or tag, the second letter of a flag. */
static BOOL JoinsPrevious(const WCHAR *text, size_t index)
{
    WCHAR c = text[index];
    WORD type = 0;
    if (IS_LOW_SURROGATE(c) || c == 0x200D || text[index - 1] == 0x200D || (c >= 0xFE00 && c <= 0xFE0F) || c == 0x20E3 ||
        IsBrahmicMark(c) || IsVirama(text[index - 1]))
        return TRUE;
    if (IsRegionalIndicatorAt(text, index)) {
        size_t before = 0;
        while (index >= 2 * (before + 1) && IsRegionalIndicatorAt(text, index - 2 * (before + 1))) before++;
        return before % 2 == 1;
    }
    if (IS_HIGH_SURROGATE(c) && IS_LOW_SURROGATE(text[index + 1])) {
        DWORD codePoint = 0x10000 + (((DWORD)c - 0xD800) << 10) + ((DWORD)text[index + 1] - 0xDC00);
        return (codePoint >= 0x1F3FB && codePoint <= 0x1F3FF) ||   /* skin tones */
               (codePoint >= 0xE0020 && codePoint <= 0xE007F) ||   /* tags (subdivision flags) */
               (codePoint >= 0xE0100 && codePoint <= 0xE01EF);     /* variation selectors */
    }
    return GetStringTypeW(CT_CTYPE3, &c, 1, &type) && (type & (C3_NONSPACING | C3_VOWELMARK)) != 0;
}

size_t Localize_ShorterCut(const WCHAR *name, size_t keep)
{
    size_t shorter = keep > 0 ? keep - 1 : 0;
    while (shorter > 0 && JoinsPrevious(name, shorter)) shorter--;
    return shorter;
}

/* `caption`: a button's or check box's text, where "&" marks the access key,
 * so the name's own ampersands are doubled (after the cut, which counts the
 * name's characters). */
static void FormatCut(const WCHAR *format, const WCHAR *name, size_t keep, BOOL caption, WCHAR *text, size_t cch)
{
    WCHAR shown[2 * LABEL_CCH + 2];
    size_t length = wcslen(name), i, n = 0;
    if (keep > length) keep = length;
    while (keep >= LABEL_CCH) keep = Localize_ShorterCut(name, keep);
    for (i = 0; i < keep; i++) {
        if (caption && name[i] == L'&') shown[n++] = L'&';
        shown[n++] = name[i];
    }
    shown[n] = 0;
    if (keep < length) StringCchCatW(shown, ARRAYSIZE(shown), L"\x2026");
    StringCchPrintfW(text, cch, format, shown);
}

void Localize_FormatCutName(const WCHAR *format, const WCHAR *name, size_t keep, WCHAR *text, size_t cch)
{
    FormatCut(format, name, keep, FALSE, text, cch);
}

void Localize_FormatCutCaption(const WCHAR *format, const WCHAR *name, size_t keep, WCHAR *text, size_t cch)
{
    FormatCut(format, name, keep, TRUE, text, cch);
}
