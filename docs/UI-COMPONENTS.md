# Shared Win32 UI components

`theme.c` owns the controls' appearance, input-related presentation and drawing resources. Feature code owns data, commands and content. The interfaces in `app.h` are the integration points; a new feature does not copy a control painter, a tooltip loop or a scroll handler.

## Native controls

Initialize the theme once with `Theme_Init`. Create controls using their native Windows classes and styles, then call `Theme_Apply(dialog)`, again after adding controls to an existing dialog. It handles:

| Control | Shared behavior |
| --- | --- |
| Push buttons and check boxes | Palette, font, hover/pressed/disabled/default states, keyboard focus and native input |
| Text edits | Native selection and caret, centered text, buffered drawing, rounded frames and scroll bars |
| Drop-down selectors | The shared drop-down painter and Windows' own menu, with a variable caption |
| Report tables and headers | Shared column geometry, rounded rows and viewport, native input, clipped labels, scroll bars |
| Trees and list boxes | Row colors, native input, pixel scrolling and clipped-text tooltips |
| Static labels and links | Palette, script-aware fonts, native links and tooltips for ellipsized labels |

`Theme_Apply` installs the dialog and control subclasses; destruction removes them and frees what they own. A dialog opened with `Ui_Dialog` is also centered on its owner, gets the theme's font and colors (`Theme_CtlColor`, unless its procedure answers `WM_CTLCOLOR*` itself), follows theme changes, and gets its own taskbar icon when its owner is hidden. A top-level window with its own procedure calls `Theme_Follow` for setting changes, as the manager does. A list box made without a frame is a side bar: its background is the window's in every mode.

`Theme_FitDialog` places a dialog's controls from its resource layout (`Theme_RememberLayout`), measuring captions in every language: a caption too wide widens the dialog and its columns in proportion, within the monitor; wrapped text grows its own row, the rows below it moving down; a row holding only hidden controls closes. A procedure hides controls in `WM_INITDIALOG` and never moves controls itself; showing a closed row later needs a new fit, and every fit starts again from the resource layout.

A tip shows the whole text of a clipped cell, tree row or ellipsized label at once, and goes when the pointer leaves it, the control scrolls or its content changes, or its window is disabled or deactivated. A tree with `TVS_INFOTIP` shows its parent's info tip (`TVN_GETINFOTIPW`) instead, as Windows' own info tips do. A tree's title is measured with its parent's custom draw, so that what the parent draws beside it counts: an item prepaint with an empty rectangle, on a memory DC, is answered with `CDRF_NEWFONT`, the title's font selected and its bounds in the rectangle.

The profile color selector and the sessions' Actions boxes share their drawing (`Theme_DrawDropDown`) and Windows' own menu (`Theme_TrackDropDown`); a drop-down list's native list never opens.

Custom button surfaces, selected rows, scrolling viewports and text edits share a four-DIP corner radius (`THEME_CORNER_RADIUS_DIPS`). Light-mode buttons use their Windows theme; high contrast keeps the system's selection and focus cues. Native client geometry, caret positions and scroll bar hit targets keep their Windows layout, except a one-line edit's client area, which starts lower so that its text sits in the middle.

Use `Theme_CheckBoxSize` for a check box's caption and glyph instead of copying glyph-size constants. A new kind of native control gets its setup, painting and lifetime in `theme.c`, with a check in `tests/test_theme.c`, rather than a feature-local painter.

## Tables and scrolling views

Keep the data control and its viewport handles separate:

```c
HWND table = GetDlgItem(dialog, IDC_TABLE);
ListView_SetExtendedListViewStyleEx(table,
    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP,
    LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
HWND viewport = Theme_SmoothView(table);
Theme_Apply(dialog);
```

Populate, select and query rows through `table`; position, hide and show the scrolling area through `viewport`. The viewport takes the control's dialog ID and frame and forwards its notifications. `Theme_SmoothView` returns an existing viewport, whether it is given the table or the viewport.

The viewport brings the keyboard's row into sight when the keyboard or the program moves it (arrows, type-ahead, Page Up and Down, a click on another row, `ListView_EnsureVisible`, `TreeView_EnsureVisible`, `LB_SETCARETINDEX`, a selection set while the control draws). Refilling, expanding a folder with its arrow and resizing keep the scroll position. A refill is what happens between `WM_SETREDRAW` FALSE and TRUE: a selection set there is not followed, so a background refill never jumps; the caller that wants the row in sight asks once drawing is back on. Ctrl with a navigation key scrolls a tree's viewport without moving its selection. A control too tall for a viewport (`THEME_VIEW_MAX_PX`, 30,000 pixels), or showing its own horizontal scroll bar (a table whose columns are wider than the viewport), scrolls itself by rows, with the same smooth wheel: its scroll bars are then at the viewport's edges, as a native list's, never at the end of its rows. The rows on top stay on top as it starts and stops.

A window of the feature's own that scrolls by the pixel handles `WM_VSCROLL` with `Theme_ScrollTarget`; `Theme_SetScrollRow` gives it the smooth wheel. The wheel's animation takes a frame at each refresh of the screen: while it runs, a clock thread waits for each composition (`DwmFlush`) and posts the next frame, once the last one was handled, so input never waits behind frames; a timer's frames (some 15.6 ms apart, and only once the queue is empty) would stutter. Each frame moves a tree or list view without copying its pixels, updates its row under the mouse, then draws the part that shows whole, off screen (the view gives it its double-buffer style): the screen shows the frame before or the next one, never a copy whose uncovered strip waits for its rows. A list box, which draws on screen, keeps the copy.

Use `Theme_SetColumnWidth` for sizing columns. A divider the user drags or double-clicks marks its column sized by hand (`Theme_ColumnResizeIsManual`), and layout keeps its width. A table's last column takes the width the others leave, never less than its title (`Theme_FitLastColumn`), in the same step as another column's change (narrower before it, wider after it, so that the columns never outgrow the list for a moment); a column whose divider the user drags follows the drag only; the profile list's Profile and Role columns start at widths common to every language (`Theme_ProfileColumnWidths`).

The report-table component owns the list's subclass and the drawing of its rows and header. Rows paint background and foreground in one pass (`CDRF_SKIPDEFAULT`) with the native item rectangles, alignment and image lists; Windows keeps the item model, input, notifications and accessibility. Everything it draws belongs to the content: the columns' places come from the header's items, and each window places them from its own scroll (the rows from where the list's rows start, the header in its own coordinates, since Windows moves the header window on its own). The rows' column separators and the header's dividers coincide, and a row and the table end at the last column's right edge, which has no separator (its divider still draggable). Windows scrolls a list and shifts its columns by copying its pixels; that drawing moves with them and stays right, so the component adds nothing to scrolling or column changes.

## Custom content

Feature-specific content uses the same primitives:

- `Theme_Color` and `Theme_Brush` give palette values, `THEME_SEPARATOR` the line between two parts of a window.
- `Theme_CreateFonts` and `Theme_FreeFonts` manage script-aware text, strong, heading and other font variants.
- `Theme_DrawRow` takes the owning HWND for its DPI, paints row state and returns its text color; `Theme_RowMuted` gives the row's secondary text color.
- `Theme_DrawTreeGlyph` paints a folder's open or closed arrow from the tree's own theme, hot with the row (`THEME_ROW_HOT`).
- `Theme_DrawFocusCue` marks the keyboard's row, after keyboard use only; a feature's custom-drawn tree or list draws it for its focused row.
- `Theme_DrawButton` and `Theme_DrawDropDown` paint interactive surfaces; `Theme_DropDownWidth` is the width a drop-down button needs for its caption, `Theme_DropDownLabel` the room its caption gets, and `Theme_TableCellText` where a table cell's text is drawn, which tips measure too.
- `Theme_TrackDropDown` tracks their native popup menus.
- `Theme_BufferBegin` and `Theme_BufferEnd` surround a complete custom draw.

Drawing and hit-testing rectangles must match: reserve counters, icons and suffixes in the geometry used to measure clipped text. Restore selected GDI objects before releasing their contexts, end each buffer on every path, and never keep a device context as a cache.

## Responsive layout

`Theme_MainMinimum` reports the main window's minimum client size, measured once per DPI and font across every interface language and its script font (a light check box's room is what the native control asks for in each language, `BCM_GETIDEALSIZE` on a hidden copy of it, the largest of them); `Theme_LayoutMain` places the controls of both views in the current client rectangle, without measuring again. What the window shows is spelled once, in `theme.c`'s measured tables, and the feature translates the key it is given: `Theme_MainCaption`, `Theme_ProfileColumnTitle`, `Theme_ProfileRole`, `Theme_MainFooter`, `Theme_MainNote` and `Theme_SessionsCaption`. Language, view and status changes keep the user's window size. The content keeps a reading width, centered in a wider window; lists take the extra height. A label beside buttons (the status, the version link) draws its text at its top while a button centers its caption, so the label is placed lower, its first line level with the captions.

Resizing does not flicker: the main window clips its children (`WS_CLIPCHILDREN`), so its background never paints over them, and every child paints all of its own pixels (an opaque link, labels filled with the `WM_CTLCOLORSTATIC` brush). `Theme_LayoutMain` moves the controls in one batch (`DeferWindowPos`), and a control whose size changes is drawn again whole (`SWP_NOCOPYBITS`), since aligned, wrapped or cut text and rounded corners depend on its size. Nothing paints in the middle of a layout: the window paints once it is done. No window is composed (`WS_EX_COMPOSITED`): `UpdateWindow` does not paint one, only the message queue does, so it would lag behind a resize.

`Gui_MainWindowGeometry` adapts `WM_GETMINMAXINFO` and `WM_DPICHANGED` to this policy; the main dialog turns off the dialog manager's own DPI scaling (`DDC_DISABLE_ALL`). `SessionsView_Resize` follows a geometry change of the sessions view, keeping its tree, expansion and selection.

## Validation and references

`tests/test_theme.c` compares what the theme draws with Windows' own drawing. `tests/test_layout.c` checks control bounds, text rendering, language changes and responsive layouts on private fixtures. Physical mouse movement, monitor changes and perceived animation still need checks by hand, on disposable fixtures, never on the running Claude profiles or the personal taskbar.

Relevant Windows contracts:

- [WM_SETREDRAW](https://learn.microsoft.com/en-us/windows/win32/gdi/wm-setredraw)
- [ScrollWindowEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-scrollwindowex)
- [Extended window styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles)
- [Window styles](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-styles)
- [DeferWindowPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-deferwindowpos)
- [Extended list-view styles](https://learn.microsoft.com/en-us/windows/win32/controls/extended-list-view-styles)
- [List-view custom draw](https://learn.microsoft.com/en-us/windows/win32/controls/nm-customdraw-list-view)
