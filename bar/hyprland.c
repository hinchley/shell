#include "hyprland.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "memory.h"
#include "strbuf.h"
#include "wayland.h"

static constexpr u32 listenerCapacity = 8;
// the event socket is reconnected this long after hyprland drops it (when it restarts).
static constexpr u64 reconnectDelay = 2'000'000;

typedef struct Hyprland {
  void (*listeners[listenerCapacity])(const char *name, const char *data);
  u32 listenerCount;
  sd_event_source *events;
  // the timer that tries the event socket again.
  sd_event_source *retry;
  // the event stream's incomplete last line.
  StrBuf partial;
} Hyprland;

static Hyprland hyprland;

// a connected socket to the instance's socket named name, or -1.
static int
Connect(const char *name, int flags)
{
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!runtime || !signature) return -1;

  struct sockaddr_un address = {.sun_family = AF_UNIX};
  snprintf(
    address.sun_path,
    sizeof(address.sun_path),
    "%s/hypr/%s/%s",
    runtime,
    signature,
    name
  );
  int file = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | flags, 0);
  if (file < 0) return -1;
  if (connect(file, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(file);
    return -1;
  }
  return file;
}

char *
HyprlandRequest(const char *request)
{
  int file = Connect(".socket.sock", 0);
  if (file < 0) return nullptr;

  u64 length = strlen(request);
  if (write(file, request, length) != (ssize_t)length) {
    close(file);
    return nullptr;
  }

  // hyprland replies, then closes the connection.
  StrBuf reply = {};
  char chunk[8192];
  ssize_t count;
  while ((count = read(file, chunk, sizeof(chunk))) > 0) {
    StrBufAppend(&reply, chunk, (u64)count);
  }
  close(file);
  if (!reply.data) StrBufAppend(&reply, "", 0);
  return reply.data;
}

void
HyprlandDispatch(const char *argument)
{
  StrBuf request = {};
  StrBufAppendFormat(&request, "dispatch %s", argument);
  free(HyprlandRequest(request.data));
  StrBufFree(&request);
}

static void
ConnectEvents(void);

static int
Reconnect(
  [[maybe_unused]] sd_event_source *source,
  [[maybe_unused]] u64 now,
  [[maybe_unused]] void *context
)
{
  ConnectEvents();
  return 0;
}

// connects to the event socket again in a little while.
static void
Retry(void)
{
  if (!hyprland.retry) {
    sd_event_add_time_relative(
      wayland.loop,
      &hyprland.retry,
      CLOCK_MONOTONIC,
      reconnectDelay,
      0,
      Reconnect,
      nullptr
    );
    return;
  }
  sd_event_source_set_time_relative(hyprland.retry, reconnectDelay);
  sd_event_source_set_enabled(hyprland.retry, SD_EVENT_ONESHOT);
}

// hands each complete line ("name>>data") to the listeners.
static int
EventsReadable(
  sd_event_source *source,
  int file,
  [[maybe_unused]] u32 events,
  [[maybe_unused]] void *context
)
{
  char chunk[8192];
  ssize_t count = read(file, chunk, sizeof(chunk));
  if (count == 0 || (count < 0 && errno != EAGAIN && errno != EINTR)) {
    // hyprland went away: try again shortly.
    sd_event_source_disable_unref(source);
    hyprland.events = nullptr;
    StrBufClear(&hyprland.partial);
    Retry();
    return 0;
  }
  if (count < 0) return 0;

  StrBufAppend(&hyprland.partial, chunk, (u64)count);
  char *line = hyprland.partial.data;
  char *end;
  while ((end = strchr(line, '\n'))) {
    *end = '\0';
    char *separator = strstr(line, ">>");
    if (separator) {
      *separator = '\0';
      for (u32 index = 0; index < hyprland.listenerCount; index++) {
        hyprland.listeners[index](line, separator + 2);
      }
    }
    line = end + 1;
  }
  StrBufRemove(&hyprland.partial, 0, (u64)(line - hyprland.partial.data));
  return 0;
}

static void
ConnectEvents(void)
{
  int file = Connect(".socket2.sock", SOCK_NONBLOCK);
  if (file < 0) {
    Retry();
    return;
  }
  sd_event_add_io(wayland.loop, &hyprland.events, file, EPOLLIN, EventsReadable, nullptr);
  sd_event_source_set_io_fd_own(hyprland.events, true);

  // events may have been missed while disconnected, so listeners are told to catch up.
  for (u32 index = 0; index < hyprland.listenerCount; index++) {
    hyprland.listeners[index]("connected", "");
  }
}

void
HyprlandListen(void (*listener)(const char *name, const char *data))
{
  if (hyprland.listenerCount == listenerCapacity) return;

  hyprland.listeners[hyprland.listenerCount++] = listener;
  if (hyprland.listenerCount == 1) ConnectEvents();
}
