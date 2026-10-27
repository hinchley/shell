#include "menu.h"

#include <linux/input-event-codes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "dbus.h"
#include "memory.h"
#include "strbuf.h"
#include "theme.h"
#include "wayland.h"

static constexpr char menuInterface[] = "com.canonical.dbusmenu";
// the deepest submenu shown.
static constexpr u32 levelCapacity = 6;

// the layout, in logical pixels, in the style of the calendar.
static constexpr f64 menuRadius = 10;
static constexpr f64 menuPadding = 6;
static constexpr f64 rowHeight = 28;
static constexpr f64 rowRadius = 6;
static constexpr f64 rowInset = 4;
static constexpr f64 textPadding = 12;
static constexpr f64 markWidth = 20;
static constexpr f64 separatorHeight = 9;
static constexpr f64 minimumWidth = 120;

static constexpr TextStyle labelStyle = {.size = themeFontSize};

// what a menu entry toggles, if anything.
enum MenuToggle {
  MenuToggleNone,
  MenuToggleCheck,
  MenuToggleRadio
};

typedef struct MenuItem {
  i32 id;
  // the label without its mnemonic underscores.
  char *label;
  bool separator;
  bool enabled;
  bool visible;
  bool submenu;
  enum MenuToggle toggle;
  bool checked;
  struct MenuItem *children;
  u32 childCount;
} MenuItem;

// one popup: a menu or submenu.
typedef struct Level {
  Surface *surface;
  // the entry whose children it shows.
  MenuItem *item;
  // the child under the pointer, or -1.
  i32 hovered;
} Level;

typedef struct Menu {
  BarOutput *output;
  Box anchor;
  char *service;
  char *path;
  MenuItem root;
  Level levels[levelCapacity];
  u32 depth;
  // counts menus opened, so a reply for one since replaced is ignored.
  u64 generation;
} Menu;

static Menu menu;

static void
FreeItem(MenuItem *item)
{
  for (u32 index = 0; index < item->childCount; index++) FreeItem(&item->children[index]);
  free(item->children);
  free(item->label);
  *item = (MenuItem){};
}

// label without mnemonic markers: "_File" becomes "File", "__" becomes "_".
static char *
StripMnemonics(const char *label)
{
  StrBuf out = {};
  for (const char *at = label; *at; at++) {
    if (*at == '_') {
      if (at[1] != '_') continue;
      at++;
    }
    StrBufAppendByte(&out, *at);
  }
  if (!out.data) StrBufAppend(&out, "", 0);
  return out.data;
}

static bool
VisitItem(void *context, const char *name, sd_bus_message *message)
{
  MenuItem *item = context;
  StrBuf text = {};
  bool read = false;
  if (strcmp(name, "type") == 0) {
    read = DbusVariantString(message, &text);
    item->separator = strcmp(StrBufText(&text), "separator") == 0;
  } else if (strcmp(name, "label") == 0) {
    read = DbusVariantString(message, &text);
    free(item->label);
    item->label = StripMnemonics(StrBufText(&text));
  } else if (strcmp(name, "enabled") == 0) {
    read = DbusVariantBool(message, &item->enabled);
  } else if (strcmp(name, "visible") == 0) {
    read = DbusVariantBool(message, &item->visible);
  } else if (strcmp(name, "toggle-type") == 0) {
    read = DbusVariantString(message, &text);
    const char *type = StrBufText(&text);
    item->toggle = strcmp(type, "checkmark") == 0 ? MenuToggleCheck
      : strcmp(type, "radio") == 0                ? MenuToggleRadio
                                                  : MenuToggleNone;
  } else if (strcmp(name, "toggle-state") == 0) {
    i64 state = 0;
    read = DbusVariantI64(message, &state);
    item->checked = state == 1;
  } else if (strcmp(name, "children-display") == 0) {
    read = DbusVariantString(message, &text);
    item->submenu = strcmp(StrBufText(&text), "submenu") == 0;
  }
  StrBufFree(&text);
  return read;
}

// reads one (ia{sv}av) layout entry, with its children, into item.
static void
ReadItem(sd_bus_message *message, MenuItem *item)
{
  *item = (MenuItem){.enabled = true, .visible = true};
  if (sd_bus_message_enter_container(message, 'r', "ia{sv}av") <= 0) return;

  sd_bus_message_read_basic(message, 'i', &item->id);
  DbusReadProperties(message, VisitItem, item);
  if (sd_bus_message_enter_container(message, 'a', "v") > 0) {
    u32 capacity = 0;
    while (sd_bus_message_enter_container(message, 'v', "(ia{sv}av)") > 0) {
      if (item->childCount == capacity) {
        capacity = capacity ? capacity * 2 : 8;
        item->children = MemoryResize(item->children, capacity, sizeof(MenuItem));
      }
      ReadItem(message, &item->children[item->childCount++]);
      sd_bus_message_exit_container(message);
    }
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  if (item->childCount) item->submenu = true;
  if (!item->label) item->label = MemoryCopyString("");
}

// the height of a child's row (0 when hidden).
static f64
RowHeight(const MenuItem *item)
{
  if (!item->visible) return 0;
  return item->separator ? separatorHeight : rowHeight;
}

// the size of the popup listing item's children.
static void
LevelSize(const MenuItem *item, u32 *width, u32 *height)
{
  f64 widest = 0;
  bool marks = false;
  bool arrows = false;
  f64 total = 2 * menuPadding;
  for (u32 index = 0; index < item->childCount; index++) {
    const MenuItem *child = &item->children[index];
    total += RowHeight(child);
    if (!child->visible || child->separator) continue;

    Text text = TextLayout(child->label, &labelStyle, 0, 0);
    widest = fmax(widest, ceil(text.width));
    TextFree(&text);
    marks |= child->toggle != MenuToggleNone;
    arrows |= child->submenu;
  }
  f64 full = widest + 2 * textPadding + (marks ? markWidth : 0) + (arrows ? markWidth : 0);
  *width = (u32)fmax(minimumWidth, full);
  *height = (u32)total;
}

// the child of the level's item at y, or -1.
static i32
ChildAt(const Level *level, f64 y)
{
  f64 top = menuPadding;
  for (u32 index = 0; index < level->item->childCount; index++) {
    f64 height = RowHeight(&level->item->children[index]);
    if (y >= top && y < top + height) return (i32)index;
    top += height;
  }
  return -1;
}

// the top of a child's row.
static f64
ChildTop(const Level *level, u32 child)
{
  f64 top = menuPadding;
  for (u32 index = 0; index < child; index++) top += RowHeight(&level->item->children[index]);
  return top;
}

static void
CloseFrom(u32 depth)
{
  while (menu.depth > depth) {
    Level *level = &menu.levels[--menu.depth];
    SurfaceDestroy(level->surface);
    *level = (Level){};
  }
}

void
MenuClose(void)
{
  CloseFrom(0);
  menu.generation++;
  FreeItem(&menu.root);
  free(menu.service);
  free(menu.path);
  menu.service = menu.path = nullptr;
  menu.output = nullptr;
}

void
MenuCloseOn(BarOutput *output)
{
  if (menu.output == output) MenuClose();
}

static Level *
LevelOf(Surface *surface)
{
  for (u32 index = 0; index < menu.depth; index++) {
    if (menu.levels[index].surface == surface) return &menu.levels[index];
  }
  return nullptr;
}

static void
Draw([[maybe_unused]] void *context, Surface *surface, cairo_t *cairo)
{
  Level *level = LevelOf(surface);
  if (!level) return;

  DrawRoundedRect(
    cairo,
    0,
    0,
    surface->width,
    surface->height,
    menuRadius,
    themePill,
    themeBorderStrong
  );

  bool marks = false;
  for (u32 index = 0; index < level->item->childCount; index++) {
    marks |= level->item->children[index].toggle != MenuToggleNone;
  }

  f64 top = menuPadding;
  for (u32 index = 0; index < level->item->childCount; index++) {
    const MenuItem *child = &level->item->children[index];
    f64 height = RowHeight(child);
    if (height == 0) continue;

    if (child->separator) {
      DrawSetColour(cairo, themeBorderStrong);
      cairo_rectangle(
        cairo,
        textPadding,
        top + floor(height / 2),
        surface->width - 2 * textPadding,
        1
      );
      cairo_fill(cairo);
      top += height;
      continue;
    }

    if ((i32)index == level->hovered && child->enabled) {
      DrawRoundedRect(
        cairo,
        rowInset,
        top,
        surface->width - 2 * rowInset,
        height,
        rowRadius,
        themeFillHover,
        themeNone
      );
    }

    Colour colour = child->enabled ? themeFg : themeMuted;
    f64 x = textPadding;
    if (marks) {
      const char *mark = !child->checked ? "" : child->toggle == MenuToggleRadio ? "●" : "✓";
      Text text = TextLayout(mark, &labelStyle, 0, 0);
      TextDraw(cairo, &text, x, top + floor((height - ceil(text.height)) / 2), themeCyan);
      TextFree(&text);
      x += markWidth;
    }

    Text text = TextLayout(child->label, &labelStyle, 0, 0);
    TextDraw(cairo, &text, x, top + floor((height - ceil(text.height)) / 2), colour);
    TextFree(&text);

    if (child->submenu) {
      Text arrow = TextLayout("›", &labelStyle, 0, 0);
      f64 arrowX = surface->width - textPadding - ceil(arrow.width);
      TextDraw(cairo, &arrow, arrowX, top + floor((height - ceil(arrow.height)) / 2), colour);
      TextFree(&arrow);
    }
    top += height;
  }
}

static void
OpenLevel(Surface *parent, Box anchor, MenuItem *item, enum PopupPlacement placement);

// tells the item's owner that the entry was chosen, and closes the menu.
static void
Choose(const MenuItem *item)
{
  sd_bus *bus = BarUserBus();
  if (bus && menu.service) {
    sd_bus_call_method_async(
      bus,
      nullptr,
      menu.service,
      menu.path,
      menuInterface,
      "Event",
      nullptr,
      nullptr,
      "isvu",
      item->id,
      "clicked",
      "i",
      0,
      (u32)time(nullptr)
    );
  }
  MenuClose();
}

// opens the submenu of the level's hovered child, closing any deeper one.
static void
OpenSubmenu(u32 depth)
{
  Level *level = &menu.levels[depth];
  CloseFrom(depth + 1);
  if (level->hovered < 0) return;

  MenuItem *child = &level->item->children[level->hovered];
  if (!child->submenu || !child->enabled || !child->childCount) return;
  if (depth + 1 >= levelCapacity) return;

  f64 top = ChildTop(level, (u32)level->hovered);
  Box row = {0, top - menuPadding, level->surface->width, rowHeight};
  OpenLevel(level->surface, row, child, PopupPlacementBeside);
}

static void
PointerMotion([[maybe_unused]] void *context, Surface *surface, f64 x, f64 y)
{
  Level *level = LevelOf(surface);
  if (!level) return;

  i32 hovered = x >= 0 && x < surface->width ? ChildAt(level, y) : -1;
  if (hovered >= 0) {
    const MenuItem *child = &level->item->children[hovered];
    if (child->separator || !child->enabled) hovered = -1;
  }
  SurfaceSetCursor(
    surface,
    hovered >= 0 ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER
                 : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT
  );
  if (hovered == level->hovered) return;

  level->hovered = hovered;
  SurfaceRedraw(surface);
  // hovering an entry with a submenu opens it, and hovering another closes it.
  if (hovered >= 0) OpenSubmenu((u32)(level - menu.levels));
}

static void
PointerLeave([[maybe_unused]] void *context, Surface *surface)
{
  Level *level = LevelOf(surface);
  if (!level || level->hovered < 0) return;

  // a submenu keeps its parent entry lit.
  bool open = (u32)(level - menu.levels) + 1 < menu.depth;
  if (open) return;
  level->hovered = -1;
  SurfaceRedraw(surface);
}

static void
PointerButton(
  [[maybe_unused]] void *context,
  Surface *surface,
  u32 button,
  bool pressed,
  [[maybe_unused]] u32 time
)
{
  Level *level = LevelOf(surface);
  if (!level || pressed || (button != BTN_LEFT && button != BTN_RIGHT) || level->hovered < 0) {
    return;
  }

  MenuItem *child = &level->item->children[level->hovered];
  if (child->submenu) OpenSubmenu((u32)(level - menu.levels));
  else Choose(child);
}

static void
Key(
  [[maybe_unused]] void *context,
  Surface *surface,
  u32 keysym,
  [[maybe_unused]] u32 codepoint,
  [[maybe_unused]] u32 modifiers
)
{
  Level *level = LevelOf(surface);
  if (!level || keysym != XKB_KEY_Escape) return;

  // escape closes the innermost menu.
  u32 depth = (u32)(level - menu.levels);
  if (depth) CloseFrom(depth);
  else MenuClose();
}

static void
Closed([[maybe_unused]] void *context, Surface *surface)
{
  Level *level = LevelOf(surface);
  if (!level) return;

  u32 depth = (u32)(level - menu.levels);
  if (depth) CloseFrom(depth);
  else MenuClose();
}

static const SurfaceHandlers handlers = {
  .Draw = Draw,
  .Closed = Closed,
  .PointerMotion = PointerMotion,
  .PointerLeave = PointerLeave,
  .PointerButton = PointerButton,
  .Key = Key
};

static void
OpenLevel(Surface *parent, Box anchor, MenuItem *item, enum PopupPlacement placement)
{
  u32 width;
  u32 height;
  LevelSize(item, &width, &height);
  PopupOptions options = {
    .parent = parent,
    .anchorX = (i32)anchor.x,
    .anchorY = (i32)anchor.y,
    .anchorWidth = (i32)anchor.width,
    .anchorHeight = (i32)anchor.height,
    .placement = placement,
    .width = width,
    .height = height,
    .grab = true
  };
  Level *level = &menu.levels[menu.depth++];
  *level = (Level){.item = item, .hovered = -1};
  level->surface = SurfaceCreatePopup(&options, &handlers, nullptr);
}

static int
GotLayout(sd_bus_message *reply, void *context, [[maybe_unused]] sd_bus_error *error)
{
  u64 generation = (u64)(uintptr_t)context;
  if (generation != menu.generation || !menu.output || !menu.output->surface) return 0;
  if (sd_bus_message_is_method_error(reply, nullptr)) {
    MenuClose();
    return 0;
  }

  u32 revision;
  sd_bus_message_read_basic(reply, 'u', &revision);
  FreeItem(&menu.root);
  ReadItem(reply, &menu.root);
  if (!menu.root.childCount) {
    MenuClose();
    return 0;
  }
  OpenLevel(menu.output->surface, menu.anchor, &menu.root, PopupPlacementBelow);
  return 0;
}

void
MenuOpen(BarOutput *output, Box anchor, const char *service, const char *path)
{
  MenuClose();
  sd_bus *bus = BarUserBus();
  if (!bus || !output->surface || !wayland.wmBase) return;

  menu.output = output;
  menu.anchor = anchor;
  menu.service = MemoryCopyString(service);
  menu.path = MemoryCopyString(path);

  // the owner is told the menu is about to show (so it can update it), then the whole layout
  // is read.
  sd_bus_call_method_async(
    bus,
    nullptr,
    service,
    path,
    menuInterface,
    "AboutToShow",
    nullptr,
    nullptr,
    "i",
    0
  );
  sd_bus_call_method_async(
    bus,
    nullptr,
    service,
    path,
    menuInterface,
    "GetLayout",
    GotLayout,
    (void *)(uintptr_t)menu.generation,
    "iias",
    0,
    -1,
    0
  );
}
