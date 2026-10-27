#include "calendar.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "draw.h"
#include "theme.h"
#include "wayland.h"

// the layout, in logical pixels, as the quickshell calendar had it.
static constexpr u32 popupWidth = 320;
static constexpr u32 popupHeight = 356;
static constexpr f64 popupRadius = 10;
static constexpr f64 margin = 14;
static constexpr f64 rowGap = 10;
static constexpr f64 buttonWidth = 28;
static constexpr f64 buttonHeight = 24;
static constexpr f64 buttonRadius = 8;
static constexpr f64 headerSpacing = 8;
static constexpr f64 weekdayHeight = 22;
static constexpr f64 cellSpacing = 4;
static constexpr f64 cellRadius = 8;
// the popup's top overlaps the pill by this much.
static constexpr i32 overlap = 6;

static constexpr TextStyle titleStyle = {.size = themeFontSize + 2, .bold = true};
static constexpr TextStyle arrowStyle = {.size = themeFontSize + 4, .bold = true};
static constexpr TextStyle weekdayStyle = {.size = themeFontSize - 1, .bold = true};
static constexpr TextStyle dayStyle = {.size = themeFontSize};
static constexpr TextStyle todayStyle = {.size = themeFontSize, .bold = true};

static const char *const weekdays[] = {"M", "T", "W", "T", "F", "S", "S"};

// the month buttons, which are what hovering and clicking concern.
enum CalendarButton {
  CalendarButtonNone,
  CalendarButtonPrevious,
  CalendarButtonNext
};

typedef struct Calendar {
  Surface *surface;
  BarOutput *output;
  // the month shown: the year, and the month from 0.
  i32 year;
  i32 month;
  enum CalendarButton hovered;
  enum CalendarButton pressed;
} Calendar;

static Calendar calendar;

static Box
PreviousBox(void)
{
  return (Box){margin, margin, buttonWidth, buttonHeight};
}

static Box
NextBox(void)
{
  return (Box){popupWidth - margin - buttonWidth, margin, buttonWidth, buttonHeight};
}

static enum CalendarButton
ButtonAt(f64 x, f64 y)
{
  if (BoxContains(PreviousBox(), x, y)) return CalendarButtonPrevious;
  if (BoxContains(NextBox(), x, y)) return CalendarButtonNext;
  return CalendarButtonNone;
}

// shows the month that is offset months from the one shown.
static void
ShowMonth(i32 offset)
{
  i32 months = calendar.year * 12 + calendar.month + offset;
  calendar.year = months / 12;
  calendar.month = months % 12;
  SurfaceRedraw(calendar.surface);
}

static void
DrawButton(cairo_t *cairo, Box box, const char *arrow, bool hovered)
{
  Colour fill = hovered ? themeFillHover : themeFillSubtle;
  DrawRoundedRect(
    cairo,
    box.x,
    box.y,
    box.width,
    box.height,
    buttonRadius,
    fill,
    themeBorderMed
  );
  Text text = TextLayout(arrow, &arrowStyle, 0, 0);
  // centred on whole pixels, rounding down as qt does.
  f64 x = box.x + floor((box.width - ceil(text.width)) / 2);
  f64 y = box.y + floor((box.height - ceil(text.height)) / 2);
  TextDraw(cairo, &text, x, y, themeFg);
  TextFree(&text);
}

static void
Draw([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface, cairo_t *cairo)
{
  DrawRoundedRect(
    cairo,
    0,
    0,
    popupWidth,
    popupHeight,
    popupRadius,
    themePill,
    themeBorderStrong
  );

  // the header: the month buttons either side of the month's name.
  struct tm first = {.tm_year = calendar.year - 1900, .tm_mon = calendar.month, .tm_mday = 1};
  mktime(&first);
  char title[64];
  strftime(title, sizeof(title), "%B %Y", &first);
  Text titleText = TextLayout(title, &titleStyle, 0, 0);
  f64 headerHeight = fmax(buttonHeight, ceil(titleText.height));
  f64 titleLeft = margin + buttonWidth + headerSpacing;
  f64 titleWidth = popupWidth - 2 * titleLeft;
  TextDraw(
    cairo,
    &titleText,
    titleLeft + round((titleWidth - titleText.width) / 2),
    margin + round((headerHeight - ceil(titleText.height)) / 2),
    themeCyan
  );
  TextFree(&titleText);

  Box previous = PreviousBox();
  Box next = NextBox();
  previous.y = next.y = margin + round((headerHeight - buttonHeight) / 2);
  DrawButton(cairo, previous, "‹", calendar.hovered == CalendarButtonPrevious);
  DrawButton(cairo, next, "›", calendar.hovered == CalendarButtonNext);

  // the weekday initials, in seven equal columns.
  f64 contentWidth = popupWidth - 2 * margin;
  f64 columnWidth = (contentWidth - 6 * cellSpacing) / 7;
  f64 weekdayTop = margin + headerHeight + rowGap;
  for (u32 column = 0; column < 7; column++) {
    Text text = TextLayout(weekdays[column], &weekdayStyle, 0, 0);
    f64 left = margin + column * (columnWidth + cellSpacing);
    f64 x = round(left + (columnWidth - text.width) / 2);
    f64 y = round(weekdayTop + (weekdayHeight - text.height) / 2);
    TextDraw(cairo, &text, x, y, themeMuted);
    TextFree(&text);
  }

  // six weeks of days, from the monday on or before the first.
  f64 gridTop = weekdayTop + weekdayHeight + rowGap;
  f64 rowHeight = (popupHeight - margin - gridTop - 5 * cellSpacing) / 6;
  i32 offset = (first.tm_wday + 6) % 7;
  time_t now = time(nullptr);
  struct tm today;
  localtime_r(&now, &today);

  for (u32 cell = 0; cell < 42; cell++) {
    struct tm date = {
      .tm_year = calendar.year - 1900,
      .tm_mon = calendar.month,
      .tm_mday = 1 - offset + (i32)cell,
      .tm_hour = 12
    };
    mktime(&date);
    bool inMonth = date.tm_mon == calendar.month;
    bool isToday = date.tm_year == today.tm_year && date.tm_yday == today.tm_yday;
    f64 opacity = inMonth ? 1 : 0.35;

    u32 column = cell % 7;
    u32 row = cell / 7;
    f64 x = round(margin + column * (columnWidth + cellSpacing));
    f64 y = round(gridTop + row * (rowHeight + cellSpacing));
    f64 width = round(margin + column * (columnWidth + cellSpacing) + columnWidth) - x;
    f64 height = round(gridTop + row * (rowHeight + cellSpacing) + rowHeight) - y;
    Colour fill = isToday ? themeCyan : themeFillFaint;
    Colour border = isToday ? themeNone : themeBorderSoft;
    DrawRoundedRect(
      cairo,
      x,
      y,
      width,
      height,
      cellRadius,
      ThemeFade(fill, opacity),
      ThemeFade(border, opacity)
    );

    char number[4];
    snprintf(number, sizeof(number), "%d", date.tm_mday);
    Text text = TextLayout(number, isToday ? &todayStyle : &dayStyle, 0, 0);
    Colour colour = ThemeFade(isToday ? themeBg : themeFg, opacity);
    // centred in the cell's unrounded column, as qt's grid places it, then put on a pixel.
    f64 columnLeft = margin + column * (columnWidth + cellSpacing);
    TextDraw(
      cairo,
      &text,
      round(columnLeft + (columnWidth - text.width) / 2),
      y + floor((height - ceil(text.height)) / 2),
      colour
    );
    TextFree(&text);
  }
}

static void
Close(void)
{
  SurfaceDestroy(calendar.surface);
  calendar.surface = nullptr;
  calendar.output = nullptr;
}

static void
Closed([[maybe_unused]] void *context, [[maybe_unused]] Surface *surface)
{
  Close();
}

static void
PointerMotion([[maybe_unused]] void *context, Surface *surface, f64 x, f64 y)
{
  enum CalendarButton hovered = ButtonAt(x, y);
  SurfaceSetCursor(
    surface,
    hovered ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
  );
  if (hovered == calendar.hovered) return;

  calendar.hovered = hovered;
  SurfaceRedraw(surface);
}

static void
PointerLeave([[maybe_unused]] void *context, Surface *surface)
{
  calendar.pressed = CalendarButtonNone;
  if (!calendar.hovered) return;

  calendar.hovered = CalendarButtonNone;
  SurfaceRedraw(surface);
}

static void
PointerButton(
  [[maybe_unused]] void *context,
  [[maybe_unused]] Surface *surface,
  u32 button,
  bool pressed,
  [[maybe_unused]] u32 time
)
{
  if (button != BTN_LEFT) return;
  if (pressed) {
    calendar.pressed = calendar.hovered;
    return;
  }

  if (calendar.pressed && calendar.pressed == calendar.hovered) {
    ShowMonth(calendar.pressed == CalendarButtonPrevious ? -1 : 1);
  }
  calendar.pressed = CalendarButtonNone;
}

static void
Key(
  [[maybe_unused]] void *context,
  [[maybe_unused]] Surface *surface,
  u32 keysym,
  [[maybe_unused]] u32 codepoint,
  [[maybe_unused]] u32 modifiers
)
{
  if (keysym == XKB_KEY_Escape) Close();
}

static const SurfaceHandlers handlers = {
  .Draw = Draw,
  .Closed = Closed,
  .PointerMotion = PointerMotion,
  .PointerLeave = PointerLeave,
  .PointerButton = PointerButton,
  .Key = Key
};

void
CalendarToggle(BarOutput *output, Box anchor)
{
  if (calendar.surface) {
    Close();
    return;
  }
  if (!output->surface || !wayland.wmBase) return;

  // it opens on the current month.
  time_t now = time(nullptr);
  struct tm today;
  localtime_r(&now, &today);
  calendar.year = today.tm_year + 1900;
  calendar.month = today.tm_mon;
  calendar.hovered = calendar.pressed = CalendarButtonNone;
  calendar.output = output;

  PopupOptions options = {
    .parent = output->surface,
    .anchorX = (i32)anchor.x,
    .anchorY = (i32)anchor.y,
    .anchorWidth = (i32)anchor.width,
    .anchorHeight = (i32)anchor.height,
    .offsetY = -overlap,
    .width = popupWidth,
    .height = popupHeight,
    .grab = true
  };
  calendar.surface = SurfaceCreatePopup(&options, &handlers, nullptr);
}

void
CalendarCloseOn(BarOutput *output)
{
  if (calendar.output == output) Close();
}

void
CalendarRefresh(void)
{
  if (calendar.surface) SurfaceRedraw(calendar.surface);
}
