// network: the connection in use.
// a wired connection (a physical ethernet interface that is up) comes first and shows its
// interface's name; otherwise a wifi connection shows its network's name (from iwd); otherwise
// "offline".
// the name is left off on a compact bar.
// a click opens impala.
//
// the quickshell version ran a script given the interfaces' names, and showed "offline" once
// they stopped being passed to it.
// here the interfaces are found, and the wifi details come from iwd over d-bus instead of from
// parsing iwctl's output.

#include <dirent.h>
#include <linux/input-event-codes.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dbus.h"
#include "module.h"
#include "process.h"
#include "strbuf.h"

static constexpr char service[] = "net.connman.iwd";
// the arp hardware type of ethernet, as /sys/class/net/<interface>/type gives it.
static constexpr i32 typeEther = 1;

enum NetworkKind {
  NetworkKindUnknown,
  NetworkKindOffline,
  NetworkKindEthernet,
  NetworkKindWifi
};

typedef struct Network {
  enum NetworkKind kind;
  // the wired interface, or the wifi network's name.
  StrBuf name;

  // the wired interface that is up, if any.
  StrBuf wired;
  // iwd's connected network, if any.
  StrBuf wifi;
} Network;

static Network network;

// the first line of the file at path, without its newline.
// false when it cannot be read.
static bool
ReadLine(const char *path, char *line, u64 size)
{
  FILE *file = fopen(path, "re");
  if (!file) return false;

  bool read = fgets(line, (int)size, file) != nullptr;
  fclose(file);
  if (read) line[strcspn(line, "\n")] = '\0';
  return read;
}

// decides what to show from the wired and wifi state.
static void
Update(void)
{
  enum NetworkKind kind = NetworkKindOffline;
  const StrBuf *name = nullptr;
  if (network.wired.length) {
    kind = NetworkKindEthernet;
    name = &network.wired;
  } else if (network.wifi.length) {
    kind = NetworkKindWifi;
    name = &network.wifi;
  }

  const char *text = name ? StrBufText(name) : "";
  if (kind == network.kind && strcmp(text, StrBufText(&network.name)) == 0) return;
  network.kind = kind;
  StrBufClear(&network.name);
  StrBufAppendString(&network.name, text);
  BarRedraw();
}

// finds the first (by name) physical ethernet interface that is up.
static void
CheckWired(void)
{
  StrBuf wired = {};
  DIR *directory = opendir("/sys/class/net");
  struct dirent *item;
  while (directory && (item = readdir(directory))) {
    const char *name = item->d_name;
    if (name[0] == '.') continue;

    // a physical interface has a device; bridges, tunnels and the like do not.
    char path[512];
    struct stat status;
    snprintf(path, sizeof(path), "/sys/class/net/%s/device", name);
    if (stat(path, &status) != 0) continue;
    snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
    if (stat(path, &status) == 0) continue;

    char value[32];
    snprintf(path, sizeof(path), "/sys/class/net/%s/type", name);
    if (!ReadLine(path, value, sizeof(value)) || atoi(value) != typeEther) continue;
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", name);
    if (!ReadLine(path, value, sizeof(value)) || strcmp(value, "up") != 0) continue;

    if (!wired.length || strcmp(name, wired.data) < 0) {
      StrBufClear(&wired);
      StrBufAppendString(&wired, name);
    }
  }
  if (directory) closedir(directory);

  StrBufFree(&network.wired);
  network.wired = wired;
  Update();
}

// a link changed (a cable plugged in, an interface coming up): check again.
static int
LinkChanged(
  [[maybe_unused]] sd_event_source *source,
  int file,
  [[maybe_unused]] u32 events,
  [[maybe_unused]] void *context
)
{
  char buffer[8192];
  while (recv(file, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {}
  BarSoon(CheckWired);
  return 0;
}

// what one GetManagedObjects reply from iwd says, while it is read.
typedef struct Reading {
  StrBuf state;
  StrBuf connectedNetwork;
  StrBuf name;
  // the network objects' paths and names, as "path\tname\n" lines.
  StrBuf networks;
  // the connected station's network, by object path.
  StrBuf connected;
} Reading;

static bool
VisitStation(void *context, const char *name, sd_bus_message *message)
{
  Reading *reading = context;
  if (strcmp(name, "State") == 0) return DbusVariantString(message, &reading->state);
  if (strcmp(name, "ConnectedNetwork") == 0) {
    return DbusVariantString(message, &reading->connectedNetwork);
  }
  return false;
}

static bool
VisitNetwork(void *context, const char *name, sd_bus_message *message)
{
  Reading *reading = context;
  if (strcmp(name, "Name") == 0) return DbusVariantString(message, &reading->name);
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
      if (strcmp(interface, "net.connman.iwd.Station") == 0) {
        StrBufClear(&reading.state);
        StrBufClear(&reading.connectedNetwork);
        DbusReadProperties(reply, VisitStation, &reading);
        bool connected = strcmp(StrBufText(&reading.state), "connected") == 0;
        if (connected && reading.connectedNetwork.length && !reading.connected.length) {
          StrBufAppendString(&reading.connected, reading.connectedNetwork.data);
        }
      } else if (strcmp(interface, "net.connman.iwd.Network") == 0) {
        StrBufClear(&reading.name);
        DbusReadProperties(reply, VisitNetwork, &reading);
        StrBufAppendFormat(&reading.networks, "%s\t%s\n", path, StrBufText(&reading.name));
      } else {
        sd_bus_message_skip(reply, "a{sv}");
      }
      sd_bus_message_exit_container(reply);
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_exit_container(reply);
  }

  // the connected network's name, looked up by its path.
  StrBufClear(&network.wifi);
  u64 length = reading.connected.length;
  for (const char *at = StrBufText(&reading.networks); *at && length;) {
    const char *end = strchr(at, '\n');
    if (strncmp(at, reading.connected.data, length) == 0 && at[length] == '\t') {
      const char *name = at + length + 1;
      StrBufAppend(&network.wifi, name, (u64)(end - name));
      break;
    }
    at = end + 1;
  }

  StrBufFree(&reading.state);
  StrBufFree(&reading.connectedNetwork);
  StrBufFree(&reading.name);
  StrBufFree(&reading.networks);
  StrBufFree(&reading.connected);
  Update();
  return 0;
}

static void
CheckWifi(void)
{
  sd_bus *bus = BarSystemBus();
  if (!bus) return;

  sd_bus_message *call =
    DbusMethod(bus, service, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects");
  DbusSend(bus, call, GotObjects, nullptr);
}

// anything iwd says, or iwd starting or stopping.
static int
WifiChanged(
  [[maybe_unused]] sd_bus_message *message,
  [[maybe_unused]] void *context,
  [[maybe_unused]] sd_bus_error *error
)
{
  BarSoon(CheckWifi);
  return 0;
}

static void
Start(void)
{
  // the kernel announces link changes to this socket.
  int file = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
  struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK};
  if (file >= 0 && bind(file, (struct sockaddr *)&address, sizeof(address)) == 0) {
    sd_event_source *source;
    sd_event_add_io(wayland.loop, &source, file, EPOLLIN, LinkChanged, nullptr);
    sd_event_source_set_io_fd_own(source, true);
  } else if (file >= 0) {
    close(file);
  }

  sd_bus *bus = BarSystemBus();
  if (bus) {
    DbusWatchSender(bus, service, WifiChanged);
    DbusWatchName(bus, service, WifiChanged);
  }
  CheckWired();
  CheckWifi();
}

static u32
Segments(const BarOutput *output, Segment *segments)
{
  static char text[256];
  const char *icon = network.kind == NetworkKindEthernet ? "\U000F0200" : "\U000F0928";
  switch (network.kind) {
    case NetworkKindUnknown:
      return 0;
    case NetworkKindOffline:
      snprintf(text, sizeof(text), "offline");
      break;
    default:
      if (output->compact) snprintf(text, sizeof(text), "%s", icon);
      else snprintf(text, sizeof(text), "%s %s", icon, StrBufText(&network.name));
      break;
  }
  segments[0] = SegmentLabel(text, themeBlue);
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
  static const char *const program[] = {"impala", nullptr};
  if (button == BTN_LEFT) ProcessTui("impala", program);
}

const BarModule networkModule = {
  .name = "network",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .padding = 30,
  .spacing = 4,
  .clickable = true
};
