#include "picker.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw.h"
#include "input.h"
#include "memory.h"
#include "strbuf.h"
#include "theme.h"
#include "wayland.h"

// the layout, in logical pixels, as the quickshell PickerWindow had it.
static constexpr f64 panelRadius = 10;
static constexpr f64 panelMargin = 14;
static constexpr f64 columnSpacing = 10;
static constexpr f64 fieldHeight = 42;
static constexpr f64 fieldRadius = 10;
static constexpr f64 fieldPadding = 12;
static constexpr f64 rowPadding = 12;
static constexpr f64 rowSpacing = 16;
static constexpr f64 rowHeightSingle = 46;
static constexpr f64 rowHeightMinimum = 36;
static constexpr f64 highlightRadius = 8;
// how far one wheel notch scrolls the list.
static constexpr f64 wheelStep = 60;
// the highlight's glide to a new row, in microseconds.
static constexpr u64 highlightMove = 80'000;
static constexpr u64 caretBlink = 500'000;
// the longest gap between the presses of a double click, in milliseconds.
static constexpr u32 doubleClickTime = 400;

static constexpr TextStyle fieldStyle = {.size = themeFontSize + 3};
static constexpr TextStyle rowStyle = {.size = themeFontSize + 1};
static constexpr TextStyle rowStyleCurrent = {.size = themeFontSize + 1, .bold = true};
static constexpr TextStyle noMatchesStyle = {.size = themeFontSize, .bold = true};

typedef struct Picker {
  PickerOptions options;
  const PickerRow *rows;
  u64 rowCount;
  // a transparent full screen surface, taking the keyboard and the clicks that dismiss the
  // picker, and the panel drawn on it as a subsurface (so only the panel needs a buffer).
  Surface *surface;
  Surface *panel;

  // each row's height, measured the first time it is listed (0 until then).
  f64 *heights;
  // the rows listed, as indexes into rows, and the top of each in the list's content (offsets
  // has one more entry, the content's height).
  u64 *shown;
  u64 shownCount;
  f64 *offsets;
  // the selected entry of shown, or -1 when nothing is listed.
  i64 current;
  // how far the list is scrolled.
  f64 scroll;

  // the highlight as drawn, gliding from (fromY, fromHeight) to the current row.
  f64 fromY;
  f64 fromHeight;
  f64 highlightY;
  f64 highlightHeight;
  u64 moveStart;

  StrBuf query;
  // the caret's byte offset, and the other end of the selection (the same when there is none).
  u64 caret;
  u64 anchor;
  // how far the field's text is scrolled to keep the caret in view.
  f64 fieldScroll;
  bool focused;
  bool caretShown;
  sd_event_source *blink;

  // the pointer's position, and what a press began on.
  f64 pointerX;
  f64 pointerY;
  bool pressOutside;
  bool selecting;
  i64 pressRow;
  i64 lastPressRow;
  u32 lastPressTime;
} Picker;

static Picker picker;

// microseconds on the monotonic clock.
static u64
Now(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (u64)now.tv_sec * 1'000'000 + (u64)now.tv_nsec / 1000;
}

static bool
TwoColumns(void)
{
  return picker.options.primaryWidth > 0;
}

// the panel, in its own surface's coordinates.
static Box
PanelBox(void)
{
  return (Box){0, 0, picker.options.panelWidth, picker.options.panelHeight};
}

static Box
FieldBox(void)
{
  Box panel = PanelBox();
  return (Box){
    .x = panel.x + panelMargin,
    .y = panel.y + panelMargin,
    .width = panel.width - 2 * panelMargin,
    .height = fieldHeight
  };
}

// the height of the "No matches" line, which takes room from the list when shown.
static f64
NoMatchesHeight(void)
{
  if (picker.shownCount) return 0;

  Text text = TextLayout("No matches", &noMatchesStyle, 0, 0);
  f64 height = text.height;
  TextFree(&text);
  return height;
}

static Box
ListBox(void)
{
  Box panel = PanelBox();
  f64 top = panelMargin + fieldHeight + columnSpacing;
  f64 below = panelMargin;
  if (!picker.shownCount) below += NoMatchesHeight() + columnSpacing;
  return (Box){
    .x = panel.x + panelMargin,
    .y = panel.y + top,
    .width = panel.width - 2 * panelMargin,
    .height = fmax(0, panel.height - top - below)
  };
}

// the width of a two column row's second column.
static f64
SecondaryWidth(void)
{
  return picker.options.panelWidth - 2 * panelMargin - 2 * rowPadding -
    picker.options.primaryWidth - rowSpacing;
}

// a row's height: fixed for one column, else room for its wrapped second column.
static f64
RowHeight(u64 row)
{
  if (!TwoColumns()) return rowHeightSingle;
  if (picker.heights[row] > 0) return picker.heights[row];

  const char *secondary = picker.rows[row].secondary;
  Text text = TextLayout(secondary ? secondary : "", &rowStyle, SecondaryWidth(), 2);
  picker.heights[row] = fmax(rowHeightMinimum, text.height + 16);
  TextFree(&text);
  return picker.heights[row];
}

// the list's scroll limit.
static f64
MaxScroll(void)
{
  return fmax(0, picker.offsets[picker.shownCount] - ListBox().height);
}

// scrolls the list just enough to show the current row whole.
static void
ShowCurrent(void)
{
  if (picker.current < 0) return;

  f64 top = picker.offsets[picker.current];
  f64 bottom = picker.offsets[picker.current + 1];
  f64 height = ListBox().height;
  if (top < picker.scroll) picker.scroll = top;
  else if (bottom > picker.scroll + height) picker.scroll = bottom - height;
  picker.scroll = fmin(fmax(picker.scroll, 0), MaxScroll());
}

// starts the highlight's glide to the current row.
static void
MoveHighlight(void)
{
  picker.fromY = picker.highlightY;
  picker.fromHeight = picker.highlightHeight;
  picker.moveStart = Now();
}

static void
Select(i64 entry)
{
  if (!picker.shownCount) return;

  if (entry < 0) entry = 0;
  if (entry >= (i64)picker.shownCount) entry = (i64)picker.shownCount - 1;
  if (entry == picker.current) return;

  picker.current = entry;
  MoveHighlight();
  ShowCurrent();
  SurfaceRedraw(picker.panel);
}

// lists the rows matching the query, selecting the first.
static void
Filter(void)
{
  picker.shownCount =
    picker.options.Filter(picker.options.context, StrBufText(&picker.query), picker.shown);

  f64 offset = 0;
  for (u64 entry = 0; entry < picker.shownCount; entry++) {
    picker.offsets[entry] = offset;
    offset += RowHeight(picker.shown[entry]);
  }
  picker.offsets[picker.shownCount] = offset;

  picker.current = picker.shownCount ? 0 : -1;
  picker.scroll = 0;
  MoveHighlight();
}

static void
Close(void)
{
  SurfaceDestroy(picker.panel);
  SurfaceDestroy(picker.surface);
  picker.panel = picker.surface = nullptr;
  WaylandQuit(0);
}

static void
Activate(void)
{
  if (picker.current < 0 || !picker.options.Activated) return;

  u64 row = picker.shown[picker.current];
  Close();
  picker.options.Activated(picker.options.context, row);
}

// restarts the caret's blink with it shown, as after any edit or move.
static void
WakeCaret(void)
{
  picker.caretShown = true;
  if (picker.blink) {
    sd_event_source_set_time_relative(picker.blink, caretBlink);
    sd_event_source_set_enabled(picker.blink, SD_EVENT_ON);
  }
}

static int
Blink(sd_event_source *source, [[maybe_unused]] u64 now, [[maybe_unused]] void *context)
{
  picker.caretShown = !picker.caretShown;
  sd_event_source_set_time_relative(source, caretBlink);
  if (picker.panel) SurfaceRedraw(picker.panel);
  return 0;
}

// the selection's start and end offsets.
static u64
SelectionStart(void)
{
  return picker.caret < picker.anchor ? picker.caret : picker.anchor;
}

static u64
SelectionEnd(void)
{
  return picker.caret > picker.anchor ? picker.caret : picker.anchor;
}

// deletes the selected text.
// false when nothing was selected.
static bool
DeleteSelection(void)
{
  u64 start = SelectionStart();
  u64 end = SelectionEnd();
  if (start == end) return false;

  StrBufRemove(&picker.query, start, end - start);
  picker.caret = picker.anchor = start;
  return true;
}

// the offset of the character before offset.
static u64
PreviousCharacter(u64 offset)
{
  const char *text = StrBufText(&picker.query);
  if (offset == 0) return 0;
  offset--;
  while (offset > 0 && (text[offset] & 0xC0) == 0x80) offset--;
  return offset;
}

// the offset of the character after offset.
static u64
NextCharacter(u64 offset)
{
  const char *text = StrBufText(&picker.query);
  if (offset >= picker.query.length) return picker.query.length;
  offset++;
  while (offset < picker.query.length && (text[offset] & 0xC0) == 0x80) offset++;
  return offset;
}

// the start of the word before offset, skipping spaces first.
static u64
PreviousWord(u64 offset)
{
  const char *text = StrBufText(&picker.query);
  while (offset > 0 && text[offset - 1] == ' ') offset--;
  while (offset > 0 && text[offset - 1] != ' ') offset--;
  return offset;
}

// the end of the word after offset, skipping spaces first.
static u64
NextWord(u64 offset)
{
  const char *text = StrBufText(&picker.query);
  while (offset < picker.query.length && text[offset] == ' ') offset++;
  while (offset < picker.query.length && text[offset] != ' ') offset++;
  return offset;
}

// the query changed: filter again.
static void
Edited(void)
{
  WakeCaret();
  Filter();
  SurfaceRedraw(picker.panel);
}

static void
Insert(const char *text, u64 length)
{
  DeleteSelection();
  StrBufInsert(&picker.query, picker.caret, text, length);
  picker.caret = picker.anchor = picker.caret + length;
  Edited();
}

// moves the caret, extending the selection when extend is set.
static void
MoveCaret(u64 offset, bool extend)
{
  picker.caret = offset;
  if (!extend) picker.anchor = offset;
  WakeCaret();
  SurfaceRedraw(picker.panel);
}

static void
Pasted([[maybe_unused]] void *context, const char *text)
{
  if (!text || !picker.panel) return;

  // a query is one line, so the paste stops at its first line break.
  u64 length = strcspn(text, "\r\n");
  if (length) Insert(text, length);
}

// how many rows a page key moves: a screenful, less one.
static i64
PageRows(void)
{
  f64 height = picker.current >= 0
    ? picker.offsets[picker.current + 1] - picker.offsets[picker.current]
    : rowHeightSingle;
  f64 rows = floor(ListBox().height / height) - 1;
  return rows < 1 ? 1 : (i64)rows;
}

static void
Key(
  [[maybe_unused]] void *context,
  [[maybe_unused]] Surface *surface,
  u32 keysym,
  u32 codepoint,
  u32 modifiers
)
{
  bool control = modifiers & InputModifierControl;
  bool shift = modifiers & InputModifierShift;
  // where the arrow keys move the caret: to the selection's edge when there is a selection
  // that is not being extended, otherwise a character (or, with control, a word) along.
  bool collapse = !shift && SelectionStart() != SelectionEnd();
  u64 left = collapse ? SelectionStart()
    : control         ? PreviousWord(picker.caret)
                      : PreviousCharacter(picker.caret);
  u64 right = collapse ? SelectionEnd()
    : control          ? NextWord(picker.caret)
                       : NextCharacter(picker.caret);

  switch (keysym) {
    case XKB_KEY_Escape:
      Close();
      return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      Activate();
      return;
    case XKB_KEY_Up:
      Select(picker.current - 1);
      return;
    case XKB_KEY_Down:
      Select(picker.current + 1);
      return;
    case XKB_KEY_Page_Up:
      Select(picker.current - PageRows());
      return;
    case XKB_KEY_Page_Down:
      Select(picker.current + PageRows());
      return;
    case XKB_KEY_BackSpace:
      if (DeleteSelection()) {
        Edited();
      } else if (picker.caret > 0) {
        u64 start = control ? PreviousWord(picker.caret) : PreviousCharacter(picker.caret);
        StrBufRemove(&picker.query, start, picker.caret - start);
        picker.caret = picker.anchor = start;
        Edited();
      }
      return;
    case XKB_KEY_Delete:
      if (DeleteSelection()) {
        Edited();
      } else if (picker.caret < picker.query.length) {
        u64 end = control ? NextWord(picker.caret) : NextCharacter(picker.caret);
        StrBufRemove(&picker.query, picker.caret, end - picker.caret);
        Edited();
      }
      return;
    case XKB_KEY_Left:
      MoveCaret(left, shift);
      return;
    case XKB_KEY_Right:
      MoveCaret(right, shift);
      return;
    case XKB_KEY_Home:
      MoveCaret(0, shift);
      return;
    case XKB_KEY_End:
      MoveCaret(picker.query.length, shift);
      return;
    default:
      break;
  }

  if (control) {
    switch (keysym) {
      case XKB_KEY_p:
        Select(picker.current - 1);
        break;
      case XKB_KEY_n:
        Select(picker.current + 1);
        break;
      case XKB_KEY_c:
        Close();
        break;
      case XKB_KEY_a:
        picker.anchor = 0;
        MoveCaret(picker.query.length, true);
        break;
      case XKB_KEY_w:
        picker.anchor = PreviousWord(picker.caret);
        if (DeleteSelection()) Edited();
        break;
      case XKB_KEY_u:
        picker.anchor = 0;
        if (DeleteSelection()) Edited();
        break;
      case XKB_KEY_v:
        InputPaste(Pasted, nullptr);
        break;
      default:
        break;
    }
    return;
  }

  if (!codepoint) return;

  // the codepoint as utf-8.
  char bytes[4];
  u64 length;
  if (codepoint < 0x80) {
    bytes[0] = (char)codepoint;
    length = 1;
  } else if (codepoint < 0x800) {
    bytes[0] = (char)(0xC0 | codepoint >> 6);
    bytes[1] = (char)(0x80 | (codepoint & 0x3F));
    length = 2;
  } else if (codepoint < 0x10000) {
    bytes[0] = (char)(0xE0 | codepoint >> 12);
    bytes[1] = (char)(0x80 | (codepoint >> 6 & 0x3F));
    bytes[2] = (char)(0x80 | (codepoint & 0x3F));
    length = 3;
  } else {
    bytes[0] = (char)(0xF0 | codepoint >> 18);
    bytes[1] = (char)(0x80 | (codepoint >> 12 & 0x3F));
    bytes[2] = (char)(0x80 | (codepoint >> 6 & 0x3F));
    bytes[3] = (char)(0x80 | (codepoint & 0x3F));
    length = 4;
  }
  Insert(bytes, length);
}

static void
KeyboardFocus([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface, bool focused)
{
  picker.focused = focused;
  WakeCaret();
  SurfaceRedraw(picker.panel);
}

// the field's text laid out (the placeholder is drawn separately).
static Text
QueryText(void)
{
  return TextLayout(StrBufText(&picker.query), &fieldStyle, 0, 0);
}

// the list entry under y, or -1.
static i64
EntryAt(f64 y)
{
  Box list = ListBox();
  f64 content = y - list.y + picker.scroll;
  for (u64 entry = 0; entry < picker.shownCount; entry++) {
    if (content >= picker.offsets[entry] && content < picker.offsets[entry + 1]) {
      return (i64)entry;
    }
  }
  return -1;
}

// the caret offset under x in the field.
static u64
OffsetAt(f64 x)
{
  Text text = QueryText();
  u64 offset = TextOffsetAt(&text, x - FieldBox().x - fieldPadding + picker.fieldScroll);
  TextFree(&text);
  return offset;
}

static void
PointerMotion([[maybe_unused]] void *context, Surface *surface, f64 x, f64 y)
{
  picker.pointerX = x;
  picker.pointerY = y;

  if (picker.selecting) {
    MoveCaret(OffsetAt(x), true);
    return;
  }

  u32 cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
  if (BoxContains(FieldBox(), x, y)) {
    cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
  } else if (BoxContains(ListBox(), x, y) && EntryAt(y) >= 0) {
    cursor = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
  }
  SurfaceSetCursor(surface, cursor);
}

static void
PointerButton(
  [[maybe_unused]] void *context,
  [[maybe_unused]] Surface *surface,
  u32 button,
  bool pressed,
  u32 time
)
{
  if (button != BTN_LEFT) return;

  f64 x = picker.pointerX;
  f64 y = picker.pointerY;
  if (!pressed) {
    picker.selecting = false;
    // a click selects the row it began and ended on.
    if (picker.pressRow >= 0 && BoxContains(ListBox(), x, y) &&
      EntryAt(y) == picker.pressRow) {
      Select(picker.pressRow);
    }
    return;
  }

  picker.pressRow = -1;
  if (BoxContains(FieldBox(), x, y)) {
    picker.selecting = true;
    MoveCaret(OffsetAt(x), false);
    return;
  }
  if (!BoxContains(ListBox(), x, y)) return;

  picker.pressRow = EntryAt(y);
  bool doubled = picker.pressRow >= 0 && picker.pressRow == picker.lastPressRow &&
    time - picker.lastPressTime < doubleClickTime;
  picker.lastPressRow = picker.pressRow;
  picker.lastPressTime = time;
  if (!doubled) return;

  picker.current = picker.pressRow;
  Activate();
}

// a click off the panel (pressed and released there) dismisses the picker.
static void
OutsideButton(
  [[maybe_unused]] void *context,
  [[maybe_unused]] Surface *surface,
  u32 button,
  bool pressed,
  [[maybe_unused]] u32 time
)
{
  if (button != BTN_LEFT) return;
  if (pressed) {
    picker.pressOutside = true;
    return;
  }
  if (picker.pressOutside) Close();
}

static void
OutsidePointerMotion(
  [[maybe_unused]] void *context,
  Surface *surface,
  [[maybe_unused]] f64 x,
  [[maybe_unused]] f64 y
)
{
  SurfaceSetCursor(surface, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
}

static void
PointerScroll([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface, i32 value120)
{
  if (!BoxContains(ListBox(), picker.pointerX, picker.pointerY)) return;

  f64 scroll = picker.scroll + value120 / 120.0 * wheelStep;
  scroll = fmin(fmax(scroll, 0), MaxScroll());
  if (scroll == picker.scroll) return;

  picker.scroll = scroll;
  SurfaceRedraw(picker.panel);
}

static void
DrawField(cairo_t *cairo)
{
  Box field = FieldBox();
  Colour border = picker.focused ? themeCyan : themeBorderMed;
  DrawRoundedRect(
    cairo,
    field.x,
    field.y,
    field.width,
    field.height,
    fieldRadius,
    themeFillInput,
    border
  );

  f64 textX = field.x + fieldPadding;
  f64 textWidth = field.width - 2 * fieldPadding;
  cairo_save(cairo);
  cairo_rectangle(cairo, textX, field.y, textWidth, field.height);
  cairo_clip(cairo);

  if (!picker.query.length) {
    Text placeholder = TextLayout(picker.options.placeholder, &fieldStyle, 0, 0);
    f64 y = field.y + (field.height - placeholder.height) / 2;
    TextDraw(cairo, &placeholder, textX, y, themeMuted);
    picker.fieldScroll = 0;
    if (picker.focused && picker.caretShown) {
      DrawSetColour(cairo, themeFg);
      cairo_rectangle(cairo, textX, y, 1, placeholder.height);
      cairo_fill(cairo);
    }
    TextFree(&placeholder);
    cairo_restore(cairo);
    return;
  }

  Text text = QueryText();
  f64 caretX = TextCaretAt(&text, picker.caret);
  // scroll the text sideways just enough to keep the caret in view.
  if (caretX - picker.fieldScroll > textWidth - 1) picker.fieldScroll = caretX - textWidth + 1;
  if (caretX < picker.fieldScroll) picker.fieldScroll = caretX;
  picker.fieldScroll = fmax(0, fmin(picker.fieldScroll, fmax(0, text.width - textWidth + 1)));

  f64 x = textX - picker.fieldScroll;
  f64 y = field.y + (field.height - text.height) / 2;
  u64 start = SelectionStart();
  u64 end = SelectionEnd();
  if (start != end) {
    f64 left = TextCaretAt(&text, start);
    f64 right = TextCaretAt(&text, end);
    DrawSetColour(cairo, themeCyan);
    cairo_rectangle(cairo, x + left, y, right - left, text.height);
    cairo_fill(cairo);
  }
  TextDraw(cairo, &text, x, y, themeFg);

  if (start != end) {
    // the selected text again, in the selected text colour, clipped to the selection.
    f64 left = TextCaretAt(&text, start);
    f64 right = TextCaretAt(&text, end);
    cairo_save(cairo);
    cairo_rectangle(cairo, x + left, y, right - left, text.height);
    cairo_clip(cairo);
    TextDraw(cairo, &text, x, y, themeBg);
    cairo_restore(cairo);
  } else if (picker.focused && picker.caretShown) {
    DrawSetColour(cairo, themeFg);
    cairo_rectangle(cairo, x + caretX, y, 1, text.height);
    cairo_fill(cairo);
  }
  TextFree(&text);
  cairo_restore(cairo);
}

// advances the highlight's glide, asking for another frame until it arrives.
static void
StepHighlight(void)
{
  if (picker.current < 0) return;

  f64 targetY = picker.offsets[picker.current];
  f64 targetHeight = picker.offsets[picker.current + 1] - targetY;
  f64 progress = (f64)(Now() - picker.moveStart) / (f64)highlightMove;
  if (progress >= 1 || picker.fromHeight == 0) {
    picker.highlightY = targetY;
    picker.highlightHeight = targetHeight;
    return;
  }

  // ease out: fast at first, settling onto the row.
  f64 eased = 1 - (1 - progress) * (1 - progress);
  picker.highlightY = picker.fromY + (targetY - picker.fromY) * eased;
  picker.highlightHeight = picker.fromHeight + (targetHeight - picker.fromHeight) * eased;
  SurfaceRedraw(picker.panel);
}

static void
DrawRow(cairo_t *cairo, Box list, u64 entry)
{
  const PickerRow *row = &picker.rows[picker.shown[entry]];
  bool current = (i64)entry == picker.current;
  f64 top = list.y + picker.offsets[entry] - picker.scroll;
  f64 height = picker.offsets[entry + 1] - picker.offsets[entry];
  const TextStyle *style = current ? &rowStyleCurrent : &rowStyle;
  f64 x = list.x + rowPadding;

  f64 primaryWidth = TwoColumns() ? picker.options.primaryWidth : list.width - 2 * rowPadding;
  Text primary = TextLayout(row->primary, style, primaryWidth, 0);
  Colour colour = current || TwoColumns() ? themeCyan : themeFg;
  TextDraw(cairo, &primary, x, top + (height - primary.height) / 2, colour);
  TextFree(&primary);

  if (!TwoColumns()) return;

  const char *text = row->secondary ? row->secondary : "";
  Text secondary = TextLayout(text, style, SecondaryWidth(), 2);
  f64 secondaryX = x + picker.options.primaryWidth + rowSpacing;
  colour = row->muted ? themeMuted : themeFg;
  TextDraw(cairo, &secondary, secondaryX, top + (height - secondary.height) / 2, colour);
  TextFree(&secondary);
}

static void
DrawList(cairo_t *cairo)
{
  Box list = ListBox();
  cairo_save(cairo);
  cairo_rectangle(cairo, list.x, list.y, list.width, list.height);
  cairo_clip(cairo);

  StepHighlight();
  if (picker.current >= 0) {
    DrawRoundedRect(
      cairo,
      list.x,
      list.y + picker.highlightY - picker.scroll,
      list.width,
      picker.highlightHeight,
      highlightRadius,
      ThemeFade(themeCyan, 0.18),
      ThemeFade(themeCyan, 0.28)
    );
  }

  for (u64 entry = 0; entry < picker.shownCount; entry++) {
    if (picker.offsets[entry + 1] <= picker.scroll) continue;
    if (picker.offsets[entry] >= picker.scroll + list.height) break;
    DrawRow(cairo, list, entry);
  }
  cairo_restore(cairo);

  if (picker.shownCount) return;

  Box panel = PanelBox();
  Text text = TextLayout("No matches", &noMatchesStyle, 0, 0);
  f64 x = panel.x + (panel.width - text.width) / 2;
  f64 y = panel.y + panel.height - panelMargin - text.height;
  TextDraw(cairo, &text, x, y, themeMuted);
  TextFree(&text);
}

static void
Draw([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface, cairo_t *cairo)
{
  Box panel = PanelBox();
  DrawRoundedRect(
    cairo,
    panel.x,
    panel.y,
    panel.width,
    panel.height,
    panelRadius,
    themePill,
    themeBorderStrong
  );
  DrawField(cairo);
  DrawList(cairo);
}

// the screen's size is known (or changed): centre the panel on it.
static void
Resized([[maybe_unused]] void *context, Surface *surface)
{
  i32 x = (i32)round((surface->width - picker.options.panelWidth) / 2);
  i32 y = (i32)round((surface->height - picker.options.panelHeight) / 2);
  SurfaceMove(picker.panel, x, y);
}

static void
Closed([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface)
{
  Close();
}

static const SurfaceHandlers screenHandlers = {
  .Resized = Resized,
  .Closed = Closed,
  .PointerMotion = OutsidePointerMotion,
  .PointerButton = OutsideButton,
  .KeyboardFocus = KeyboardFocus,
  .Key = Key
};

static const SurfaceHandlers panelHandlers = {
  .Draw = Draw,
  .PointerMotion = PointerMotion,
  .PointerButton = PointerButton,
  .PointerScroll = PointerScroll
};

void
PickerOpen(const PickerOptions *options, const PickerRow *rows, u64 rowCount)
{
  picker = (Picker){
    .options = *options,
    .rows = rows,
    .rowCount = rowCount,
    .heights = MemoryAllocateZeroed(rowCount, sizeof(f64)),
    .shown = MemoryResize(nullptr, rowCount, sizeof(u64)),
    .offsets = MemoryResize(nullptr, rowCount + 1, sizeof(f64)),
    .current = -1,
    .pressRow = -1,
    .lastPressRow = -1,
    .caretShown = true
  };
  Filter();

  // the whole screen, less any bar's exclusive zone, so a click anywhere off the panel
  // dismisses it.
  LayerOptions layer = {
    .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
    .keyboard = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE,
    .nameSpace = program_invocation_short_name
  };
  picker.surface = SurfaceCreateLayer(&layer, &screenHandlers, nullptr);
  picker.surface->blank = true;
  picker.panel = SurfaceCreateSubsurface(
    picker.surface,
    0,
    0,
    (u32)picker.options.panelWidth,
    (u32)picker.options.panelHeight,
    &panelHandlers,
    nullptr
  );
  sd_event_add_time_relative(
    wayland.loop,
    &picker.blink,
    CLOCK_MONOTONIC,
    caretBlink,
    0,
    Blink,
    nullptr
  );
  sd_event_source_set_enabled(picker.blink, SD_EVENT_ON);
}
