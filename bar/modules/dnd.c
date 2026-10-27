// dnd: mako's do-not-disturb mode, as a bell that is struck through (and dimmed) while the
// mode is on.
// a click turns the mode on or off.
// hidden on a compact bar.

#include <linux/input-event-codes.h>
#include <string.h>

#include "dbus.h"
#include "module.h"
#include "strbuf.h"

static constexpr char service[] = "org.freedesktop.Notifications";
static constexpr char makoPath[] = "/fr/emersion/Mako";
static constexpr char makoInterface[] = "fr.emersion.Mako";
static constexpr char mode[] = "do-not-disturb";

typedef struct Dnd {
  // mako's active modes, one per line.
  StrBuf modes;
  bool enabled;
} Dnd;

static Dnd dnd;

// whether the newline separated list holds item.
static bool
ListHas(const char *list, const char *item)
{
  u64 length = strlen(item);
  for (const char *at = list; *at;) {
    u64 lineLength = strcspn(at, "\n");
    if (lineLength == length && strncmp(at, item, length) == 0) return true;
    at += lineLength;
    if (*at) at++;
  }
  return false;
}

static int
GotModes(
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  if (sd_bus_message_is_method_error(reply, nullptr)) {
    StrBufClear(&dnd.modes);
  } else {
    DbusVariantStrings(reply, &dnd.modes, "\n");
  }

  bool enabled = ListHas(StrBufText(&dnd.modes), mode);
  if (enabled == dnd.enabled) return 0;
  dnd.enabled = enabled;
  BarRedraw();
  return 0;
}

static void
Fetch(void)
{
  sd_bus *bus = BarUserBus();
  if (!bus) return;

  sd_bus_message *call =
    DbusMethod(bus, service, makoPath, "org.freedesktop.DBus.Properties", "Get");
  if (call) sd_bus_message_append(call, "ss", makoInterface, "Modes");
  DbusSend(bus, call, GotModes, nullptr);
}

// mako's modes changed (it only says they did), or mako started or stopped.
static int
Changed(
  [[maybe_unused]] sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  Fetch();
  return 0;
}

static void
Start(void)
{
  sd_bus *bus = BarUserBus();
  if (!bus) return;

  sd_bus_match_signal(
    bus,
    nullptr,
    nullptr,
    makoPath,
    "org.freedesktop.DBus.Properties",
    "PropertiesChanged",
    Changed,
    nullptr
  );
  DbusWatchName(bus, service, Changed);
  Fetch();
}

static u32
Segments(const BarOutput *output, Segment *segments)
{
  if (output->compact) return 0;

  segments[0] = SegmentLabel(
    dnd.enabled ? "\U000F009B" : "\U000F009A",
    dnd.enabled ? themeMuted : themePurple
  );
  return 1;
}

// sets mako's modes to the current ones with do-not-disturb added or removed.
static void
Click(
  [[maybe_unused]] BarOutput *output,
  [[maybe_unused]] Box pill,
  [[maybe_unused]] Box segment,
  [[maybe_unused]] u32 action,
  u32 button
)
{
  sd_bus *bus = BarUserBus();
  if (button != BTN_LEFT || !bus) return;

  sd_bus_message *call = DbusMethod(bus, service, makoPath, makoInterface, "SetModes");
  if (!call) return;
  sd_bus_message_open_container(call, 'a', "s");
  const char *at = StrBufText(&dnd.modes);
  while (*at) {
    u64 length = strcspn(at, "\n");
    StrBuf item = {};
    StrBufAppend(&item, at, length);
    if (strcmp(item.data, mode) != 0) sd_bus_message_append_basic(call, 's', item.data);
    StrBufFree(&item);
    at += length;
    if (*at) at++;
  }
  if (!dnd.enabled) sd_bus_message_append_basic(call, 's', mode);
  sd_bus_message_close_container(call);
  DbusSend(bus, call, Changed, nullptr);
}

const BarModule dndModule = {
  .name = "dnd",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
