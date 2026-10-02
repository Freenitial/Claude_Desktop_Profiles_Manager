"""Check the embedded interface catalog against the code that uses it.

Catalog rows: sorted unique keys, one column per language of src/localize.c,
no empty text, no "..." (use U+2026).
Each translation against its key: the same printf conversions in the same
order (no stray "%", no %a, %A or %n), the same number of line breaks, the
same leading and trailing spaces and line breaks (Chinese may drop the
spaces), as many ellipses, the same text after a tab (a key's own name where
Windows names it so), the same product names and access-key count.
Each language: its own quotation marks, opened and closed in turn; French
no-break spaces before : ; ? ! and inside guillemets; no straight
apostrophe outside English; Hindi and Bengali sentences end with a danda;
Arabic, Urdu and Chinese use their own question mark, comma and semicolon
(and Chinese its colon); in Arabic and Urdu a path or %APPDATA% sits in a
left-to-right embedding, "claude://" and every other inserted %s are followed
by a left-to-right mark, as is the "*.lnk" pattern; one access-key
convention per language ("(&X)" with the English key's letter, or the key
in the words), Arabic keys on letters typed without Shift.
Code: every TR() call and resource label has a key, a TR() of something else
than a literal is listed below, every key is used; controls shown together
never share an access key; the words of one command are the same wherever
it shows.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
LITERAL = re.compile(r'L"((?:\\.|[^"\\])*)"')
PART = r'(?:L"(?:\\.|[^"\\])*"|APP_NAME)'
STRING = re.compile(PART + r'(?:\s+' + PART + r')*')
CALL = re.compile(r'TR\(\s*(' + PART + r'(?:\s+' + PART + r')*)\s*\)')
TOKEN = re.compile(r'L"((?:\\.|[^"\\])*)"|\b(APP_NAME)\b')
CODE_NOISE = re.compile(r'L?"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)
APP_NAME = "Claude Desktop Profiles Manager"
FORMAT = re.compile(r'%%|%(?:[-+ #0]*)(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:I64|ll|l|h|z|w)?[diuoxXfFeEgGaAcCsSpn]')
FORBIDDEN_CONVERSIONS = ("a", "A", "n")
# Names that are no format: their single percent signs are literal.
PLAIN_NAMES = ["%LOCALAPPDATA%", "%APPDATA%"]
FIXED_TERMS = ["claude://", "%APPDATA%", "%LOCALAPPDATA%", "*.lnk", "<a href=", "</a>",
               APP_NAME, "Claude Desktop", "Claude Code", "Claude", "Cowork", "Anthropic", "Windows"]
MARKUP = re.compile(r'<a href="[^"]*">|</a>')
ELLIPSIS = "\u2026"
NBSP = "\u00a0"
LRM, LRE, PDF = "\u200e", "\u202a", "\u202c"

# Each language's opening and closing quotation marks.
QUOTES = {"en": "\u201c\u201d", "fr": "\u00ab\u00bb", "de": "\u201e\u201c", "es": "\u00ab\u00bb", "ar": "\u00ab\u00bb",
          "zh-CN": "\u201c\u201d", "ru": "\u00ab\u00bb", "hi": "\u201c\u201d", "pt-BR": "\u201c\u201d", "bn": "\u201c\u201d",
          "id": "\u201c\u201d", "ur": "\u201c\u201d"}
ALL_QUOTES = '"\u201c\u201d\u201e\u00ab\u00bb'
DANDA_LANGUAGES = {"hi", "bn"}
# ASCII punctuation these languages write with their own characters.
OWN_PUNCTUATION = {"ar": "?;,", "ur": "?;,", "zh-CN": "?;,!:"}
RIGHT_TO_LEFT = {"ar", "ur"}
# Arabic (101) letters typed with Shift: an access key there would need Alt+Shift, Windows' layout switch.
SHIFTED_ARABIC = "\u0625\u0623\u0622"
# A key named after a tab: its name in Windows' own menus of a language (test_localize.c reads it in
# Windows' keyboard layouts).
KEY_NAMES = {"Del": {"fr": "Suppr", "de": "Entf", "es": "Supr"}}

# Commands that show in several places, in the same words.
COMMAND_FAMILIES = [
    ["Rename", "&Rename\u2026", "&Rename\u2026\tF2"],
    ["&Copy to", "&Copy to\u2026"],
    ["S&hare with", "&Share with\u2026"],
    ["Delete session everywhere\u2026", "&Delete session everywhere\u2026"],
    ["Remove", "Re&move\u2026%s"],
    ["Delete", "&Delete\u2026"],
    ["New profile", "&New profile\u2026"],
    ["Uninstall", "&Uninstall\u2026"],
]

# TR() calls whose argument is no literal, by file: the variable or the
# function giving the key (an entry followed by "(" matches any call of it).
NONLITERAL_CALLS = {
    "gui.c": ["error", "g_ColorNames[i]", "title", "Theme_MainCaption", "Theme_ProfileRole", "Theme_MainFooter", "Theme_MainNote"],
    "sessions.c": ["Theme_SessionsCaption"],
    "localize.c": ["key"],
    "profiles.c": ["invalid"],
}
# Keys the code translates through a variable, by the source file that spells
# them: the color names (g_ColorNames), the main window's captions, column
# titles and roles (theme.c measures them, gui.c shows them) and the name
# errors of Core_ValidateNewName and Core_ValidateLabel.
TRANSLATED_THROUGH_VARIABLES = {
    "icons.c": ["Blue", "Green", "Orange", "Pink", "Purple", "Red", "Slate", "Teal"],
    "core.c": ["Claude uses this name itself. Pick another one.", "Enter a name.", "Name too long.",
               "The name cannot start or end with a dot.", "The name contains an invalid character.",
               "Use 32 characters or fewer.", "Use 48 characters or fewer.",
               "Use letters, digits, spaces, \u201c-\u201d, \u201c_\u201d or \u201c.\u201d."],
    "gui.c": ["A profile folder with this name already exists."],
    "theme.c": ["&Open", "&New profile\u2026", "&Edit\u2026", "&Delete\u2026", "Set as de&fault",
                "Create shortcut on des&ktop", "Shortcut on desktop", "Create s&hortcut\u2026", "Pin to &taskbar", "Pinned",
                "Add to Start &menu", "Remove from Start &menu", "&Sessions  >", "<  &Back", "&Language\u2026",
                "&Get Claude", "Set up l&inks", "&Uninstall\u2026", "U&pdate", "Close",
                "Profile", "Role", "Data folder", "Claude icon, default", "Claude icon", "Default",
                "Version %s \u00b7 by <a href=\"%s\">Freenitial</a>, not affiliated with Anthropic",
                "Version %s \u00b7 version %s is available", "Downloading version %s\u2026", "Installing the new version\u2026",
                "The default profile opens claude:// links while Claude is closed.\n\nThe regular Claude icon opens \u201c%s\u201d.",
                "Actions", "Delete session everywhere\u2026"],
}

# Controls shown at the same time: no two of them may share an access key.
# Close goes by Esc and has none (Windows' rule for OK, Cancel and Close).
STATUS_ACTIONS = ["&Get Claude", "Set up l&inks"]
ACCESS_KEY_GROUPS = {
    "profiles view": ["&Sessions  >", "&Language\u2026", "&Open", "&New profile\u2026", "&Edit\u2026", "&Delete\u2026",
                      "Set as de&fault", "Create shortcut on des&ktop", "Create s&hortcut\u2026", "Pin to &taskbar",
                      "Add to Start &menu", "Remove from Start &menu", "&Uninstall\u2026", "U&pdate"] + STATUS_ACTIONS,
    "sessions view": ["<  &Back", "&Language\u2026", "Show &archived", "&Uninstall\u2026", "U&pdate"] + STATUS_ACTIONS,
    "profile dialog": ["&Name:", "&Color:", "Open at &Windows sign-in", "Copy &settings from \u201c%s\u201d",
                       "&Open it now to sign in"],
    "Actions menu of a profile without the session": ["&Share with\u2026", "&Copy to\u2026"],
    "Actions menu of a profile with the session": ["&Open in %s", "&Rename\u2026", "S&tar", "Uns&tar",
                                                   "Re&move from %s\u2026%s", "Re&move\u2026%s", "&Keep in %s"],
    "session menu": ["&Open in %s", "Open i&n", "S&hare with", "&Copy to", "&Rename\u2026\tF2", "S&tar",
                     "Uns&tar", "Re&move from %s\u2026%s", "Re&move\u2026%s", "&Keep in %s", "&Show folder",
                     "&Delete session everywhere\u2026"],
}
# Choices made in the same place: a key of one choice never shows with a key of another.
ALTERNATIVES = [
    [{"S&tar"}, {"Uns&tar"}],
    [{"Re&move from %s\u2026%s"}, {"Re&move\u2026%s"}],
    [{"&Keep in %s"}, {"&Rename\u2026", "&Rename\u2026\tF2", "S&tar", "Uns&tar",
                       "Re&move from %s\u2026%s", "Re&move\u2026%s"}],
    [{"Add to Start &menu"}, {"Remove from Start &menu"}],
    [{"&Get Claude"}, {"Set up l&inks"}],
]
RESOURCE_STATEMENT = re.compile(r'\s*(PUSHBUTTON|DEFPUSHBUTTON|PUSHBOX|LTEXT|RTEXT|CTEXT|GROUPBOX|CHECKBOX|AUTOCHECKBOX|'
                                r'RADIOBUTTON|AUTORADIOBUTTON|STATE3|AUTO3STATE|CONTROL|CAPTION)\s+')
RESOURCE_STRING = re.compile(r'L?"((?:\\.|[^"\\])*)"')
# The classes whose labels Localize_Window translates.
TRANSLATED_CLASSES = {"button", "static"}

def decode(value):
    return re.sub(r'\\(x[0-9a-fA-F]+|[nrt\\"\'])', lambda m: chr(int(m[1][1:], 16)) if m[1].startswith("x") else {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "'": "'"}[m[1]], value)

def expression(value):
    return "".join(decode(m[1]) if m[1] is not None else APP_NAME for m in TOKEN.finditer(value))

def access_key(text):
    i = 0
    while True:
        i = text.find("&", i)
        if i < 0 or i + 1 >= len(text):
            return None
        if text[i + 1] != "&":
            return text[i + 1].upper()
        i += 2

def shown_together(a, b):
    for choices in ALTERNATIVES:
        first = [i for i, choice in enumerate(choices) if a in choice]
        second = [i for i, choice in enumerate(choices) if b in choice]
        if first and second and first[0] != second[0]:
            return False
    return True

def is_format(text):
    for name in PLAIN_NAMES:
        text = text.replace(name, "")
    return "%" in text

def format_problems(text):
    """Stray percent signs and forbidden conversions of a printf format."""
    problems = []
    position = 0
    for match in FORMAT.finditer(text):
        if "%" in text[position:match.start()]:
            problems.append("stray %")
        if match[0][-1] in FORBIDDEN_CONVERSIONS and match[0] != "%%":
            problems.append(f"conversion {match[0]}")
        position = match.end()
    if "%" in text[position:]:
        problems.append("stray %")
    return problems

def edges(text):
    """The spaces and line breaks a text starts and ends with."""
    return re.match(r"[ \n]*", text)[0], re.search(r"[ \n]*$", text)[0]

def edges_match(key, text, language):
    """Chinese writes no space next to its own words: it may drop the key's."""
    return all(text_edge == key_edge or (language == "zh-CN" and text_edge == key_edge.replace(" ", ""))
               for key_edge, text_edge in zip(edges(key), edges(text)))

def quotes_alternate(text, marks):
    """Opening and closing marks in turn, every quotation closed."""
    opening, closing = marks
    inside = False
    for c in text:
        if c == opening and not inside:
            inside = True
        elif c == closing and inside:
            inside = False
        elif c in marks:
            return False
    return not inside

def direction_problems(text):
    """Arabic and Urdu: what must read left to right inside right-to-left text."""
    problems = []
    depth = 0
    for i, c in enumerate(text):
        if c == LRE:
            depth += 1
        elif c == PDF:
            depth -= 1
        elif text.startswith("%s", i) and depth == 0 and not text.startswith(LRM, i + 2):
            problems.append("%s without a left-to-right mark after it")
        elif text.startswith("claude://", i) and not text.startswith(LRM, i + len("claude://")):
            problems.append("claude:// without a left-to-right mark after it")
        elif text.startswith("*.lnk", i) and not (text.startswith(LRM, i + len("*.lnk")) and text[i - 1:i] == LRM):
            problems.append("*.lnk without left-to-right marks around it")
        elif text.startswith("APPDATA", i) and depth == 0:
            problems.append("%APPDATA% outside a left-to-right embedding")
    if depth != 0:
        problems.append("unbalanced left-to-right embedding")
    return problems

def command_words(text):
    """A command's words: no access key, ellipsis, shortcut, colon or argument after the ellipsis."""
    text = text.split("\t")[0]
    text = re.sub(r"\(&.\)", "", text)
    text = re.sub(r"&(?!&)", "", text)
    text = re.sub(ELLIPSIS + r"(%s" + LRM + "?)?$", "", text)
    return text.rstrip(" :" + NBSP)

def without_code_noise(source):
    """Comments blanked (line breaks kept), string and character literals kept."""
    def blank(match):
        token = match[0]
        if token.startswith("/"):
            return re.sub(r"[^\n]", " ", token)
        return token
    return CODE_NOISE.sub(blank, source)

def call_argument(source, start):
    """The argument text of the call whose "(" is at `start`."""
    depth = 0
    for i in range(start, len(source)):
        if source[i] == "(":
            depth += 1
        elif source[i] == ")":
            depth -= 1
            if depth == 0:
                return source[start + 1:i]
    return source[start + 1:]

def allowed_nonliteral(file, argument):
    argument = re.sub(r"\s+", " ", argument.strip())
    for entry in NONLITERAL_CALLS.get(file, []):
        if argument == entry or argument.startswith(entry + "("):
            return True
    return False

def languages():
    source = (ROOT / "src" / "localize.c").read_text(encoding="utf-8-sig")
    table = re.search(r'kLanguages\[\]\s*=\s*\{(.*?)\n\};', source, re.S)
    return re.findall(r'\{\s*L"([^"]+)"', table[1]) if table else []

def check_translation(errors, number, langs, key, index, text):
    language = langs[index]
    def error(message):
        errors.append(f"catalog:{number}: {message} for {language}")
    if not text:
        error("empty translation")
        return
    if FORMAT.findall(key) != FORMAT.findall(text):
        error("format contract differs")
    if is_format(text):
        for problem in sorted(set(format_problems(text))):
            error(problem)
    if "ZXQ" in text or "QXZ" in text:
        error("unresolved placeholder")
    for fixed in FIXED_TERMS:
        if key.count(fixed) != text.count(fixed):
            error(f"{fixed!r} changed")
    if key.count("&") != text.count("&"):
        error("accelerator count changed")
    if key.count("\n") != text.count("\n"):
        error("line breaks differ")
    if not edges_match(key, text, language):
        error("leading or trailing spaces or line breaks differ")
    if key.count(ELLIPSIS) != text.count(ELLIPSIS):
        error("ellipsis dropped or added")
    if "..." in text:
        error("three dots: use \u2026")
    key_tab, text_tab = key.partition("\t")[2], text.partition("\t")[2]
    if text_tab != KEY_NAMES.get(key_tab, {}).get(language, key_tab) or ("\t" in key) != ("\t" in text):
        error("text after the tab differs")
    prose = MARKUP.sub("", text)
    marks = QUOTES.get(language, "")
    wrong = sorted({c for c in prose if c in ALL_QUOTES and c not in marks})
    if wrong:
        error(f"quotes {''.join(wrong)!r}: use {marks}")
    elif not quotes_alternate(prose, marks):
        error("quotation marks not opened and closed in turn")
    if language == "fr":
        french = prose.replace("claude://", "")
        if re.search("\u00ab(?!" + NBSP + ")|(?<!" + NBSP + ")\u00bb|(?<!" + NBSP + ")[:;?!]", french):
            error("missing no-break space before : ; ? ! or inside \u00ab \u00bb")
    if "'" in prose:
        error("straight apostrophe: use \u2019")
    if language in DANDA_LANGUAGES and ("\u09f7" in text or re.search(r"\.(\s|$)", text)):
        error("sentence not ended with a danda (U+0964)")
    if language in OWN_PUNCTUATION:
        plain = prose.replace("claude://", "")
        if any(c in OWN_PUNCTUATION[language] for c in plain):
            error(f"ASCII {OWN_PUNCTUATION[language]} punctuation")
    if language in RIGHT_TO_LEFT:
        for problem in sorted(set(direction_problems(text))):
            error(problem)
    if language == "ar" and access_key(text) and access_key(text) in SHIFTED_ARABIC:
        error("access key needs Shift on the Arabic keyboard")

def check_access_key_conventions(errors, catalog, rows, langs):
    """One convention per language: a "(&X)" suffix with the English key's
    letter, or the key inside the words, never both."""
    for index, language in enumerate(langs[1:], 1):
        kinds = {}
        for key, columns in catalog.items():
            letter = access_key(key)
            if letter is None:
                continue
            suffix = re.search(r"\(&([A-Za-z0-9])\)", columns[index])
            kinds.setdefault("suffix" if suffix else "inline", []).append(key)
            if suffix and suffix[1].upper() != letter:
                errors.append(f"catalog:{rows[key]}: {language} suffix key (&{suffix[1]}) is not the English one ({letter})")
        if len(kinds) > 1:
            fewer = min(kinds, key=lambda kind: len(kinds[kind]))
            for key in kinds[fewer]:
                errors.append(f"catalog:{rows[key]}: {language} mixes access-key conventions ({fewer} here)")

def check_command_families(errors, catalog, langs):
    for family in COMMAND_FAMILIES:
        missing = [key for key in family if key not in catalog]
        if missing:
            errors.append(f"check-localization.py: command family names a missing key {ascii(missing[0])}")
            continue
        for index, language in enumerate(langs):
            words = {command_words(catalog[key][index]) for key in family}
            if len(words) > 1:
                errors.append(f"{ascii(family[0])}: {language} names the command differently: {ascii(sorted(words))}")

def main():
    errors = []
    langs = languages()
    if not langs:
        errors.append("localize.c: kLanguages not found")
    catalog, rows = {}, {}
    previous = None
    lines = (ROOT / "src" / "localize_catalog.inc").read_text(encoding="utf8").splitlines()
    header = re.match(r'/\* Columns: (.*)\. \*/', lines[0]) if lines else None
    if not header or [code.strip() for code in header[1].split(",")] != langs:
        errors.append("catalog:1: the columns comment does not list the languages of localize.c")
    for number, line in enumerate(lines, 1):
        if not line.strip().startswith("{{"):
            continue
        columns = [decode(m[1]) for m in LITERAL.finditer(line)]
        if len(columns) != len(langs):
            errors.append(f"catalog:{number}: {len(columns)} columns, expected {len(langs)}")
            continue
        key = columns[0]
        if previous is not None and previous >= key:
            errors.append(f"catalog:{number}: unsorted or duplicate key")
        previous = key
        catalog[key] = columns
        rows[key] = number
        if "..." in key:
            errors.append(f"catalog:{number}: three dots in the key: use \u2026 (L\"...\\x2026\" in app.rc)")
        if is_format(key):
            for problem in sorted(set(format_problems(key))):
                errors.append(f"catalog:{number}: {problem} in the key")
        wrong = sorted({c for c in MARKUP.sub("", key) if c in ALL_QUOTES and c not in QUOTES["en"]})
        if wrong:
            errors.append(f"catalog:{number}: quotes {''.join(wrong)!r} in the key: use \u201c \u201d")
        for index in range(1, len(columns)):
            check_translation(errors, number, langs, key, index, columns[index])
    check_access_key_conventions(errors, catalog, rows, langs)
    check_command_families(errors, catalog, langs)

    literal_used, resource_used, variable_used = set(), set(), set()
    calls = 0
    literals = {}
    for path in sorted((ROOT / "src").glob("*.c")):
        source = without_code_noise(path.read_text(encoding="utf-8-sig"))
        literals[path.name] = {expression(m[0]) for m in STRING.finditer(source)}
        for match in re.finditer(r'\bTR\(', source):
            line = source.count("\n", 0, match.start()) + 1
            call = CALL.match(source, match.start())
            if not call:
                argument = call_argument(source, match.end() - 1)
                if not allowed_nonliteral(path.name, argument):
                    errors.append(f"{path.name}:{line}: TR() of {ascii(argument.strip())} cannot be checked: "
                                  "list it in NONLITERAL_CALLS and its keys in TRANSLATED_THROUGH_VARIABLES")
                continue
            calls += 1
            key = expression(call[1])
            literal_used.add(key)
            if key not in catalog:
                errors.append(f"{path.name}:{line}: missing key {ascii(key)}")
    # A key a test looks up must exist too, though a test alone does not keep a key in use.
    for path in sorted((ROOT / "tests").glob("*.c")):
        source = without_code_noise(path.read_text(encoding="utf-8-sig"))
        for call in CALL.finditer(source):
            key = expression(call[1])
            if key not in catalog:
                line = source.count("\n", 0, call.start()) + 1
                errors.append(f"tests/{path.name}:{line}: missing key {ascii(key)}")
    resource_labels = 0
    for line in (ROOT / "src" / "app.rc").read_text(encoding="utf-8-sig").splitlines():
        statement = RESOURCE_STATEMENT.match(line)
        if not statement:
            continue
        strings = RESOURCE_STRING.findall(line)
        if not strings or not strings[0]:
            continue
        if statement[1] == "CONTROL" and (len(strings) < 2 or strings[1].lower() not in TRANSLATED_CLASSES):
            continue
        key = decode(strings[0])
        if key == APP_NAME:
            continue
        resource_labels += 1
        resource_used.add(key)
        if "..." in key:
            errors.append(f"app.rc: three dots in {ascii(key)}: write L\"...\\x2026\"")
        elif key not in catalog:
            errors.append(f"app.rc: missing resource label {ascii(key)}")
    for file, keys in TRANSLATED_THROUGH_VARIABLES.items():
        for key in keys:
            if key not in catalog:
                errors.append(f"check-localization.py: {ascii(key)} ({file}) has no catalog row")
            elif key not in literals.get(file, set()):
                errors.append(f"check-localization.py: {ascii(key)} is no longer spelled in {file}")
            elif key in literal_used:
                errors.append(f"check-localization.py: {ascii(key)} has a literal TR() call: remove it from the list")
            variable_used.add(key)
    used = literal_used | resource_used | variable_used
    for key, number in rows.items():
        if key not in used:
            errors.append(f"catalog:{number}: unused key {ascii(key)}")

    for group, keys in ACCESS_KEY_GROUPS.items():
        for key in keys:
            if key not in catalog:
                errors.append(f"check-localization.py: access-key group {group!r} names a missing key {ascii(key)}")
        keys = [key for key in keys if key in catalog]
        for index, language in enumerate(langs):
            for first in range(len(keys)):
                for second in range(first + 1, len(keys)):
                    if not shown_together(keys[first], keys[second]):
                        continue
                    a, b = catalog[keys[first]][index], catalog[keys[second]][index]
                    if access_key(a) is not None and access_key(a) == access_key(b):
                        errors.append(f"{group}: {language} gives {ascii(a)} and {ascii(b)} the same access key")

    for error in errors:
        # A console or log in a legacy code page cannot hold every script: escape what it cannot print.
        print(error.encode("ascii", "backslashreplace").decode("ascii"))
    print(f"localization sources: {len(catalog)} keys x {len(langs)} languages, {calls} literal call sites, "
          f"{resource_labels} resource labels, {len(errors)} errors")
    return 1 if errors else 0

if __name__ == "__main__":
    sys.exit(main())
