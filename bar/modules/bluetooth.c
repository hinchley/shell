// bluetooth: whether the first adapter is powered, and how many of its devices are connected.
// a click opens bluetui.

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "dbus.h"
#include "module.h"
#include "process.h"
#include "strbuf.h"

static constexpr char service[] = "org.bluez";

typedef struct Bluetooth {
  // the adapter, by object path ("/org/bluez/hci0"); empty when there is none.
  StrBuf adapter;
  bool powered;
  u32 connected;
} Bluetooth;

static Bluetooth bluetooth;

// what one GetManagedObjects reply says, while it is read.
typedef struct Reading {
  StrBuf adapter;
  bool powered;
  // the connected devices' paths, one per line, for counting those of the adapter.
  StrBuf devices;
  bool connected;
} Reading;

static bool
VisitAdapter(void *context, const char *name, sd_bus_message *message)
{
  Reading *reading = context;
  if (strcmp(name, "Powered") == 0) return DbusVariantBool(message, &reading->powered);
  return false;
}

static bool
VisitDevice(void *context, const char *name, sd_bus_message *message)
{
  Reading *reading = context;
  if (strcmp(name, "Connected") == 0) return DbusVariantBool(message, &reading->connected);
  return false;
}

static int
GotObjects(
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  Reading reading = {};
  bool ok = !sd_bus_message_is_method_error(reply, nullptr) &&
    sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") > 0;

  while (ok && sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
    const char *path;
    sd_bus_message_read_basic(reply, 'o', &path);
    sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
      const char *interface;
      sd_bus_message_read_basic(reply, 's', &interface);
      // the adapter with the lowest path is the default one.
      bool firstAdapter = strcmp(interface, "org.bluez.Adapter1") == 0 &&
        (!reading.adapter.length || strcmp(path, reading.adapter.data) < 0);
      if (firstAdapter) {
        StrBufClear(&reading.adapter);
        StrBufAppendString(&reading.adapter, path);
        reading.powered = false;
        DbusReadProperties(reply, VisitAdapter, &reading);
      } else if (strcmp(interface, "org.bluez.Device1") == 0) {
        reading.connected = false;
        DbusReadProperties(reply, VisitDevice, &reading);
        if (reading.connected) StrBufAppendFormat(&reading.devices, "%s\n", path);
      } else {
        sd_bus_message_skip(reply, "a{sv}");
      }
      sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_exit_container(reply);
  }

  // the connected devices that belong to the adapter.
  u32 connected = 0;
  u64 length = reading.adapter.length;
  for (const char *at = StrBufText(&reading.devices); *at; at = strchr(at, '\n') + 1) {
    if (length && strncmp(at, reading.adapter.data, length) == 0 && at[length] == '/') {
      connected++;
    }
  }

  bool changed = bluetooth.powered != reading.powered || bluetooth.connected != connected;
  StrBufFree(&bluetooth.adapter);
  bluetooth.adapter = reading.adapter;
  bluetooth.powered = reading.powered;
  bluetooth.connected = connected;
  StrBufFree(&reading.devices);
  if (changed) BarRedraw();
  return 0;
}

static void
Refresh(void)
{
  sd_bus *bus = BarSystemBus();
  if (!bus) return;

  sd_bus_message *call =
    DbusMethod(bus, service, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
  DbusSend(bus, call, GotObjects, nullptr);
}

// anything bluez says (a device connecting, the adapter powering off) or bluez starting or
// stopping: read everything again, once the burst is over.
static int
Changed(
  [[maybe_unused]] sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  BarSoon(Refresh);
  return 0;
}

static void
Start(void)
{
  sd_bus *bus = BarSystemBus();
  if (!bus) return;

  DbusWatchSender(bus, service, Changed);
  DbusWatchName(bus, service, Changed);
  Refresh();
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  static char text[32];
  if (!bluetooth.powered) snprintf(text, sizeof(text), "\U000F00B2");
  else if (!bluetooth.connected) snprintf(text, sizeof(text), "\U000F00AF");
  else snprintf(text, sizeof(text), "\U000F00AF %u", bluetooth.connected);
  segments[0] = SegmentLabel(text, bluetooth.powered ? themeCyan : themeMuted);
  return 1;
}

static void
Click(
  [[maybe_unused]] BarOutput *output,
  [[maybe_unused]] Box pill,
  [[maybe_unused]] Box segment,
  [[maybe_unused]] u32 action,
  u32 button
)
{
  static const char *const program[] = {"bluetui", nullptr};
  if (button == BTN_LEFT) ProcessTui("bluetui", program);
}

const BarModule bluetoothModule = {
  .name = "bluetooth",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
