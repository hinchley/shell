// battery: the charge of the laptop's batteries, from upower's display device (which combines
// them).
// hidden when there is no battery.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dbus.h"
#include "module.h"

static constexpr char service[] = "org.freedesktop.UPower";
static constexpr char devicePath[] = "/org/freedesktop/UPower/devices/DisplayDevice";
static constexpr char deviceInterface[] = "org.freedesktop.UPower.Device";

// upower's device type for a battery, and its states that count as charging.
static constexpr u32 typeBattery = 2;
static constexpr u32 stateCharging = 1;
static constexpr u32 stateFullyCharged = 4;

// the battery icons from empty to full, a step for each tenth.
static const char *const levelIcons[] = {
  "\U000F008E",
  "\U000F007A",
  "\U000F007B",
  "\U000F007C",
  "\U000F007D",
  "\U000F007E",
  "\U000F007F",
  "\U000F0080",
  "\U000F0081",
  "\U000F0082",
  "\U000F0079"
};
static constexpr char chargingIcon[] = "\U000F0084";

typedef struct Battery {
  u32 type;
  bool present;
  bool powerSupply;
  f64 percentage;
  u32 state;
} Battery;

static Battery battery;

static bool
Visit([[maybe_unused]] void *context, const char *name, sd_bus_message *message)
{
  if (strcmp(name, "Type") == 0) return DbusVariantU32(message, &battery.type);
  if (strcmp(name, "IsPresent") == 0) return DbusVariantBool(message, &battery.present);
  if (strcmp(name, "PowerSupply") == 0) return DbusVariantBool(message, &battery.powerSupply);
  if (strcmp(name, "Percentage") == 0) return DbusVariantDouble(message, &battery.percentage);
  if (strcmp(name, "State") == 0) return DbusVariantU32(message, &battery.state);
  return false;
}

static int
GotAll(
  sd_bus_message *reply,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  if (sd_bus_message_is_method_error(reply, nullptr)) return 0;

  DbusReadProperties(reply, Visit, nullptr);
  BarRedraw();
  return 0;
}

static void
Fetch(void)
{
  sd_bus *bus = BarSystemBus();
  if (!bus) return;

  sd_bus_message *call =
    DbusMethod(bus, service, devicePath, "org.freedesktop.DBus.Properties", "GetAll");
  if (call) sd_bus_message_append(call, "s", deviceInterface);
  DbusSend(bus, call, GotAll, nullptr);
}

static int
Changed(
  sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  const char *interface;
  if (sd_bus_message_read_basic(message, 's', &interface) < 0) return 0;
  if (strcmp(interface, deviceInterface) != 0) return 0;

  DbusReadProperties(message, Visit, nullptr);
  BarRedraw();
  return 0;
}

// upower started or stopped.
static int
OwnerChanged(
  [[maybe_unused]] sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  battery = (Battery){};
  Fetch();
  BarRedraw();
  return 0;
}

static void
Start(void)
{
  sd_bus *bus = BarSystemBus();
  if (!bus) return;

  sd_bus_match_signal(
    bus,
    nullptr,
    service,
    devicePath,
    "org.freedesktop.DBus.Properties",
    "PropertiesChanged",
    Changed,
    nullptr
  );
  DbusWatchName(bus, service, OwnerChanged);
  Fetch();
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  if (battery.type != typeBattery || !battery.powerSupply || !battery.present) return 0;

  i32 percent = (i32)round(battery.percentage);
  bool charging = battery.state == stateCharging || battery.state == stateFullyCharged;
  i32 level = (i32)round(percent / 10.0);
  level = level < 0 ? 0 : level > 10 ? 10 : level;
  const char *icon = charging ? chargingIcon : levelIcons[level];
  Colour colour = charging ? themeGreen
    : percent <= 15        ? themeRed
    : percent <= 30        ? themeYellow
                           : themeGreen;

  static char text[32];
  snprintf(text, sizeof(text), "%s %d%%", icon, percent);
  segments[0] = SegmentLabel(text, colour);
  return 1;
}

const BarModule batteryModule = {
  .name = "battery",
  .Start = Start,
  .Segments = Segments,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
