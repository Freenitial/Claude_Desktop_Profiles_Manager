#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_LOCALIZE_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_LOCALIZE_H

/* English keys and immutable translations can be used by worker threads. */
void Localize_Init(void);
const WCHAR *Localize_Text(const WCHAR *english);
const WCHAR *Localize_TranslateAt(int language, const WCHAR *english);
int Localize_LanguageCount(void);
int Localize_LanguageForCode(const WCHAR *code);
const WCHAR *Localize_LanguageCode(int language);
const WCHAR *Localize_LanguageName(int language);   /* -1: following Windows, in the current language */
int Localize_CurrentLanguage(void); /* -1 follows Windows */
int Localize_EffectiveLanguage(void);
BOOL Localize_SetLanguage(int language, BOOL persist);
const WCHAR *Localize_FontFace(void);
const WCHAR *Localize_FontFaceAt(int language);
BOOL Localize_IsRTL(void);
BOOL Localize_IsRTLAt(int language);
UINT Localize_ReadingFlags(void);

/* Capture resource labels before WM_INITDIALOG fills any user data; forget
 * them at WM_DESTROY, while the labels still exist. */
void Localize_Window(HWND dialog);
void Localize_ForgetWindow(HWND dialog);

/* A sentence that names a profile, the name cut to fit: `format` (one %s)
 * with the first `keep` characters of `name`, an ellipsis after a cut one;
 * a name of LABEL_CCH characters or more is cut below that. The caption
 * variant is for a button or check box, where "&" marks the access key: the
 * name's own ampersands are doubled. Localize_ShorterCut gives the next
 * shorter cut that does not split a character as it shows: a surrogate pair,
 * a letter and its marks, a conjunct, an emoji sequence or a flag. */
void Localize_FormatCutName(const WCHAR *format, const WCHAR *name, size_t keep, WCHAR *text, size_t cch);
void Localize_FormatCutCaption(const WCHAR *format, const WCHAR *name, size_t keep, WCHAR *text, size_t cch);
size_t Localize_ShorterCut(const WCHAR *name, size_t keep);

/* Catalog introspection keeps completeness and format checks independent of UI. */
size_t Localize_CatalogCount(void);
const WCHAR *Localize_CatalogKey(size_t index);

#define TR(english) Localize_Text(english)

#endif
