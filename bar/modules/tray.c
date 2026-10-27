// tray: the system tray, showing the icons of applications' status notifier items (the
// freedesktop / kde tray protocol over d-bus).
// a left click activates an item, and a right click opens its menu.
// hidden when there are no items.
//
// items register with the status notifier watcher.
// when nothing else is the watcher, the bar becomes it, so it hears of items directly;
// otherwise it registers as a host with the watcher there and asks it for the items.

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dbus.h"
#include "memory.h"
#include "module.h"
#include "strbuf.h"
#include "tray/icon.h"
#include "tray/menu.h"

static constexpr char watcherName[] = "org.kde.StatusNotifierWatcher";
static constexpr char watcherPath[] = "/StatusNotifierWatcher";
static constexpr char itemInterface[] = "org.kde.StatusNotifierItem";
static constexpr char defaultItemPath[] = "/StatusNotifierItem";
// icons are drawn 20 pixels square, and loaded at twice that for scaled outputs.
static constexpr f64 iconSize = 20;
static constexpr u32 iconPixels = 40;

typedef struct Item {
  // the bus name the item is on and its object path, and the id the watcher knows it by ("bus
  // name" followed by "path").
  char *service;
  char *path;
  char *id;
  // the item's unique bus name, so it can be forgotten when its owner goes.
  char *owner;

  StrBuf title;
  StrBuf iconName;
  StrBuf iconThemePath;
  StrBuf menu;
  bool itemIsMenu;
  // the largest pixmap the item sent, if it sent any.
  u8 *pixels;
  i32 pixelsWidth;
  i32 pixelsHeight;
  cairo_surface_t *icon;
  // the title's first letter, shown when there is no icon.
  char letter[8];
  sd_bus_slot *signals;
} Item;

typedef struct Tray {
  Item **items;
  u64 count;
  u64 capacity;
  // the bar is the watcher (not just a host).
  bool watching;
  sd_bus_slot *watcherObject;
} Tray;

static Tray tray;

static Item *
FindItem(const char *id)
{
  for (u64 index = 0; index < tray.count; index++) {
    if (strcmp(tray.items[index]->id, id) == 0) return tray.items[index];
  }
  return nullptr;
}

static void
FreeItem(Item *item)
{
  sd_bus_slot_unref(item->signals);
  free(item->service);
  free(item->path);
  free(item->id);
  free(item->owner);
  StrBufFree(&item->title);
  StrBufFree(&item->iconName);
  StrBufFree(&item->iconThemePath);
  StrBufFree(&item->menu);
  free(item->pixels);
  if (item->icon) cairo_surface_destroy(item->icon);
  free(item);
}

// loads the item's icon from what it last said: its pixmap, else its icon name.
static void
LoadIcon(Item *item)
{
  if (item->icon) cairo_surface_destroy(item->icon);
  item->icon = nullptr;
  if (item->iconName.length) {
    item->icon = IconLoad(item->iconName.data, StrBufText(&item->iconThemePath), iconPixels);
  }
  if (!item->icon && item->pixels) {
    item->icon =
      IconFromPixels(item->pixels, item->pixelsWidth, item->pixelsHeight, iconPixels);
  }

  // the title's first character, as an uppercase letter.
  const char *title = item->title.length ? item->title.data : "?";
  u64 length = 1;
  while (title[length] && (title[length] & 0xC0) == 0x80) length++;
  snprintf(item->letter, sizeof(item->letter), "%.*s", (int)length, title);
  if (item->letter[0] >= 'a' && item->letter[0] <= 'z') item->letter[0] -= 'a' - 'A';
}

// reads an a(iiay) pixmap list, keeping the largest pixmap.
static bool
ReadPixmaps(sd_bus_message *message, Item *item)
{
  char type;
  const char *contents;
  if (sd_bus_message_peek_type(message, &type, &contents) <= 0 || type != 'v') return false;
  if (strcmp(contents, "a(iiay)") != 0) {
    sd_bus_message_skip(message, "v");
    return false;
  }

  sd_bus_message_enter_container(message, 'v', "a(iiay)");
  sd_bus_message_enter_container(message, 'a', "(iiay)");
  free(item->pixels);
  item->pixels = nullptr;
  item->pixelsWidth = item->pixelsHeight = 0;
  while (sd_bus_message_enter_container(message, 'r', "iiay") > 0) {
    i32 width;
    i32 height;
    const void *data;
    u64 size;
    sd_bus_message_read(message, "ii", &width, &height);
    sd_bus_message_read_array(message, 'y', &data, &size);
    bool valid = width > 0 && height > 0 && size >= (u64)width * (u64)height * 4;
    if (valid && width * height > item->pixelsWidth * item->pixelsHeight) {
      free(item->pixels);
      item->pixels = MemoryResize(nullptr, size, 1);
      memcpy(item->pixels, data, size);
      item->pixelsWidth = width;
      item->pixelsHeight = height;
    }
    sd_bus_message_exit_container(message);
  }
  sd_bus_message_exit_container(message);
  sd_bus_message_exit_container(message);
  return true;
}

static bool
VisitItem(void *context, const char *name, sd_bus_message *message)
{
  Item *item = context;
  if (strcmp(name, "Title") == 0) return DbusVariantString(message, &item->title);
  if (strcmp(name, "IconName") == 0) return DbusVariantString(message, &item->iconName);
  if (strcmp(name, "IconThemePath") == 0) {
    return DbusVariantString(message, &item->iconThemePath);
  }
  if (strcmp(name, "Menu") == 0) return DbusVariantString(message, &item->menu);
  if (strcmp(name, "ItemIsMenu") == 0) return DbusVariantBool(message, &item->itemIsMenu);
  if (strcmp(name, "IconPixmap") == 0) return ReadPixmaps(message, item);
  return false;
}

static int
GotProperties(sd_bus_message *reply, void *context, [[maybe_unused]] sd_bus_error *error)
{
  char *id = context;
  Item *item = FindItem(id);
  free(id);
  if (!item || sd_bus_message_is_method_error(reply, nullptr)) return 0;

  DbusReadProperties(reply, VisitItem, item);
  LoadIcon(item);
  BarRedraw();
  return 0;
}

static void
FetchItem(Item *item)
{
  sd_bus_call_method_async(
    BarUserBus(),
    nullptr,
    item->service,
    item->path,
    "org.freedesktop.DBus.Properties",
    "GetAll",
    GotProperties,
    MemoryCopyString(item->id),
    "s",
    itemInterface
  );
}

// the item changed its icon, title or status: read it again.
static int
ItemChanged(
  [[maybe_unused]] sd_bus_message *message,
  void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  Item *item = FindItem(context);
  if (item) FetchItem(item);
  return 0;
}

static int
GotOwner(sd_bus_message *reply, void *context, [[maybe_unused]] sd_bus_error *error)
{
  char *id = context;
  Item *item = FindItem(id);
  free(id);
  const char *owner;
  if (!item || sd_bus_message_read_basic(reply, 's', &owner) <= 0) return 0;

  free(item->owner);
  item->owner = MemoryCopyString(owner);
  return 0;
}

static void
RemoveItem(const char *id)
{
  for (u64 index = 0; index < tray.count; index++) {
    Item *item = tray.items[index];
    if (strcmp(item->id, id) != 0) continue;

    if (tray.watching) {
      sd_bus_emit_signal(
        BarUserBus(),
        watcherPath,
        watcherName,
        "StatusNotifierItemUnregistered",
        "s",
        item->id
      );
    }
    FreeItem(item);
    memmove(
      &tray.items[index],
      &tray.items[index + 1],
      (tray.count - index - 1) * sizeof(Item *)
    );
    tray.count--;
    BarRedraw();
    return;
  }
}

// adds the item with the watcher's id for it: a bus name, optionally followed by its path.
// owner is its unique bus name, when known.
static void
AddItem(const char *id, const char *owner)
{
  if (FindItem(id)) return;

  Item *item = MemoryAllocateZeroed(1, sizeof(Item));
  const char *slash = strchr(id, '/');
  item->id = MemoryCopyString(id);
  item->service = slash ? MemoryCopyStringLength(id, (u64)(slash - id)) : MemoryCopyString(id);
  item->path = MemoryCopyString(slash ? slash : defaultItemPath);
  snprintf(item->letter, sizeof(item->letter), "?");
  if (tray.count == tray.capacity) {
    tray.capacity = tray.capacity ? tray.capacity * 2 : 8;
    tray.items = MemoryResize(tray.items, tray.capacity, sizeof(Item *));
  }
  tray.items[tray.count++] = item;

  sd_bus *bus = BarUserBus();
  sd_bus_match_signal(
    bus,
    &item->signals,
    item->service,
    item->path,
    itemInterface,
    nullptr,
    ItemChanged,
    item->id
  );
  if (owner) {
    item->owner = MemoryCopyString(owner);
  } else {
    sd_bus_call_method_async(
      bus,
      nullptr,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "GetNameOwner",
      GotOwner,
      MemoryCopyString(id),
      "s",
      item->service
    );
  }
  FetchItem(item);

  if (tray.watching) {
    sd_bus_emit_signal(
      bus,
      watcherPath,
      watcherName,
      "StatusNotifierItemRegistered",
      "s",
      item->id
    );
    sd_bus_emit_properties_changed(
      bus,
      watcherPath,
      watcherName,
      "RegisteredStatusNotifierItems",
      nullptr
    );
  }
}

// forgets the items whose owner left the bus.
static int
OwnerChanged(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  const char *name;
  const char *before;
  const char *after;
  if (sd_bus_message_read(message, "sss", &name, &before, &after) < 0 || *after) return 0;

  for (u64 index = tray.count; index > 0; index--) {
    Item *item = tray.items[index - 1];
    bool gone = strcmp(item->service, name) == 0 ||
      (item->owner && strcmp(item->owner, before) == 0);
    if (gone) RemoveItem(item->id);
  }
  return 0;
}

// the watcher's RegisterStatusNotifierItem: the argument is the item's bus name, or (as
// libappindicator sends it) its object path on the caller's connection.
static int
RegisterItem(sd_bus_message *message, [[maybe_unused]] void *context, sd_bus_error *error)
{
  const char *argument;
  if (sd_bus_message_read_basic(message, 's', &argument) < 0) {
    return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "expected a service");
  }

  const char *sender = sd_bus_message_get_sender(message);
  StrBuf id = {};
  if (argument[0] == '/') StrBufAppendFormat(&id, "%s%s", sender, argument);
  else StrBufAppendString(&id, argument);
  AddItem(id.data, argument[0] == '/' || argument[0] == ':' ? sender : nullptr);
  StrBufFree(&id);
  return sd_bus_reply_method_return(message, "");
}

static int
RegisterHost(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  return sd_bus_reply_method_return(message, "");
}

static int
GetItems(
  [[maybe_unused]] sd_bus *bus,
  [[maybe_unused]] const char *path,
  [[maybe_unused]] const char *interface,
  [[maybe_unused]] const char *property,
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  sd_bus_message_open_container(reply, 'a', "s");
  for (u64 index = 0; index < tray.count; index++) {
    sd_bus_message_append_basic(reply, 's', tray.items[index]->id);
  }
  return sd_bus_message_close_container(reply);
}

static int
GetHostRegistered(
  [[maybe_unused]] sd_bus *bus,
  [[maybe_unused]] const char *path,
  [[maybe_unused]] const char *interface,
  [[maybe_unused]] const char *property,
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  return sd_bus_message_append(reply, "b", 1);
}

static int
GetProtocolVersion(
  [[maybe_unused]] sd_bus *bus,
  [[maybe_unused]] const char *path,
  [[maybe_unused]] const char *interface,
  [[maybe_unused]] const char *property,
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcherTable[] = {
  SD_BUS_VTABLE_START(0),
  SD_BUS_METHOD(
    "RegisterStatusNotifierItem",
    "s",
    "",
    RegisterItem,
    SD_BUS_VTABLE_UNPRIVILEGED
  ),
  SD_BUS_METHOD(
    "RegisterStatusNotifierHost",
    "s",
    "",
    RegisterHost,
    SD_BUS_VTABLE_UNPRIVILEGED
  ),
  SD_BUS_PROPERTY(
    "RegisteredStatusNotifierItems",
    "as",
    GetItems,
    0,
    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE
  ),
  SD_BUS_PROPERTY(
    "IsStatusNotifierHostRegistered",
    "b",
    GetHostRegistered,
    0,
    SD_BUS_VTABLE_PROPERTY_CONST
  ),
  SD_BUS_PROPERTY("ProtocolVersion", "i", GetProtocolVersion, 0, SD_BUS_VTABLE_PROPERTY_CONST),
  SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
  SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
  SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
  SD_BUS_VTABLE_END
};

// another watcher's announcements, when the bar is only a host.
static int
ItemRegistered(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  const char *id;
  if (sd_bus_message_read_basic(message, 's', &id) > 0) AddItem(id, nullptr);
  return 0;
}

static int
ItemUnregistered(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  const char *id;
  if (sd_bus_message_read_basic(message, 's', &id) > 0) RemoveItem(id);
  return 0;
}

static int
GotRegisteredItems(
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  if (sd_bus_message_is_method_error(reply, nullptr)) return 0;
  if (sd_bus_message_enter_container(reply, 'v', "as") <= 0) return 0;
  if (sd_bus_message_enter_container(reply, 'a', "s") <= 0) return 0;

  const char *id;
  while (sd_bus_message_read_basic(reply, 's', &id) > 0) AddItem(id, nullptr);
  return 0;
}

static void
BecomeHost(sd_bus *bus)
{
  char hostName[128];
  snprintf(hostName, sizeof(hostName), "org.kde.StatusNotifierHost-%d", getpid());
  sd_bus_request_name(bus, hostName, 0);
  sd_bus_match_signal(
    bus,
    nullptr,
    watcherName,
    watcherPath,
    watcherName,
    "StatusNotifierItemRegistered",
    ItemRegistered,
    nullptr
  );
  sd_bus_match_signal(
    bus,
    nullptr,
    watcherName,
    watcherPath,
    watcherName,
    "StatusNotifierItemUnregistered",
    ItemUnregistered,
    nullptr
  );
  sd_bus_call_method_async(
    bus,
    nullptr,
    watcherName,
    watcherPath,
    watcherName,
    "RegisterStatusNotifierHost",
    nullptr,
    nullptr,
    "s",
    hostName
  );
  sd_bus_call_method_async(
    bus,
    nullptr,
    watcherName,
    watcherPath,
    "org.freedesktop.DBus.Properties",
    "Get",
    GotRegisteredItems,
    nullptr,
    "ss",
    watcherName,
    "RegisteredStatusNotifierItems"
  );
}

static void
Start(void)
{
  sd_bus *bus = BarUserBus();
  if (!bus) return;

  sd_bus_add_match(
    bus,
    nullptr,
    "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
    "member='NameOwnerChanged'",
    OwnerChanged,
    nullptr
  );

  // be the watcher if there is none, or else a host of the one there is.
  sd_bus_add_object_vtable(
    bus,
    &tray.watcherObject,
    watcherPath,
    watcherName,
    watcherTable,
    nullptr
  );
  tray.watching = sd_bus_request_name(bus, watcherName, 0) >= 0;
  if (tray.watching) {
    sd_bus_emit_signal(bus, watcherPath, watcherName, "StatusNotifierHostRegistered", "");
    return;
  }
  tray.watcherObject = sd_bus_slot_unref(tray.watcherObject);
  BecomeHost(bus);
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  u32 count = 0;
  for (u64 index = 0; index < tray.count && count < segmentCapacity; index++) {
    const Item *item = tray.items[index];
    if (item->icon) {
      segments[count] =
        (Segment){.icon = item->icon, .iconSize = iconSize, .action = (u32)index + 1};
    } else {
      segments[count] = (Segment){
        .text = item->letter,
        .colour = themeFg,
        .sizeDelta = -1,
        .bold = true,
        .width = iconSize,
        .height = iconSize,
        .action = (u32)index + 1
      };
    }
    count++;
  }
  return count;
}

static void
Click(BarOutput *output, [[maybe_unused]] Box pill, Box segment, u32 action, u32 button)
{
  if (action == 0 || action > tray.count) return;
  Item *item = tray.items[action - 1];

  // a right click (or any click on an item that is only a menu) opens the menu.
  bool menu = button == BTN_RIGHT || (button == BTN_LEFT && item->itemIsMenu);
  if (menu && item->menu.length) {
    MenuOpen(output, segment, item->service, item->menu.data);
    return;
  }
  if (button != BTN_LEFT && button != BTN_RIGHT) return;

  sd_bus_call_method_async(
    BarUserBus(),
    nullptr,
    item->service,
    item->path,
    itemInterface,
    button == BTN_LEFT ? "Activate" : "ContextMenu",
    nullptr,
    nullptr,
    "ii",
    0,
    0
  );
}

static void
Hide(BarOutput *output)
{
  MenuCloseOn(output);
}

const BarModule trayModule = {
  .name = "tray",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .Hide = Hide,
  .padding = 15,
  .spacing = 4
};
