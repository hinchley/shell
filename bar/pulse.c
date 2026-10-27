#include "pulse.h"

#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>

#include "memory.h"
#include "types.h"
#include "wayland.h"

// libpulse marks a time given on the monotonic clock with this bit in tv_usec (its
// PA_TIMEVAL_RTCLOCK); any other time is on the wall clock.
static constexpr u64 monotonicFlag = 1ull << 30;

struct pa_io_event {
  sd_event_source *source;
  pa_io_event_cb_t callback;
  pa_io_event_destroy_cb_t destroy;
  void *context;
};

struct pa_time_event {
  sd_event_source *source;
  pa_time_event_cb_t callback;
  pa_time_event_destroy_cb_t destroy;
  void *context;
};

struct pa_defer_event {
  sd_event_source *source;
  pa_defer_event_cb_t callback;
  pa_defer_event_destroy_cb_t destroy;
  void *context;
};

static pa_mainloop_api api;

static u32
ToEpoll(pa_io_event_flags_t flags)
{
  return (flags & PA_IO_EVENT_INPUT ? EPOLLIN : 0u) |
    (flags & PA_IO_EVENT_OUTPUT ? EPOLLOUT : 0u);
}

static pa_io_event_flags_t
FromEpoll(u32 events)
{
  return (events & EPOLLIN ? PA_IO_EVENT_INPUT : 0) |
    (events & EPOLLOUT ? PA_IO_EVENT_OUTPUT : 0) |
    (events & EPOLLHUP ? PA_IO_EVENT_HANGUP : 0) | (events & EPOLLERR ? PA_IO_EVENT_ERROR : 0);
}

static int
IoReady([[maybe_unused]] sd_event_source *source, int file, u32 events, void *context)
{
  pa_io_event *event = context;
  event->callback(&api, event, file, FromEpoll(events), event->context);
  return 0;
}

static pa_io_event *
IoNew(
  [[maybe_unused]] pa_mainloop_api *self,
  int file,
  pa_io_event_flags_t flags,
  pa_io_event_cb_t callback,
  void *context
)
{
  pa_io_event *event = MemoryAllocateZeroed(1, sizeof(pa_io_event));
  event->callback = callback;
  event->context = context;
  sd_event_add_io(wayland.loop, &event->source, file, ToEpoll(flags), IoReady, event);
  if (!flags) sd_event_source_set_enabled(event->source, SD_EVENT_OFF);
  return event;
}

static void
IoEnable(pa_io_event *event, pa_io_event_flags_t flags)
{
  if (flags) sd_event_source_set_io_events(event->source, ToEpoll(flags));
  sd_event_source_set_enabled(event->source, flags ? SD_EVENT_ON : SD_EVENT_OFF);
}

static void
IoFree(pa_io_event *event)
{
  sd_event_source_disable_unref(event->source);
  if (event->destroy) event->destroy(&api, event, event->context);
  free(event);
}

static void
IoSetDestroy(pa_io_event *event, pa_io_event_destroy_cb_t destroy)
{
  event->destroy = destroy;
}

// a libpulse time as microseconds on the monotonic clock.
static u64
Monotonic(const struct timeval *time)
{
  u64 usec = (u64)time->tv_usec;
  if (usec & monotonicFlag) return (u64)time->tv_sec * 1'000'000 + (usec & ~monotonicFlag);

  // a wall clock time: keep its distance from now.
  struct timespec wall;
  struct timespec monotonic;
  clock_gettime(CLOCK_REALTIME, &wall);
  clock_gettime(CLOCK_MONOTONIC, &monotonic);
  i64 target = (i64)time->tv_sec * 1'000'000 + (i64)usec;
  i64 now = (i64)wall.tv_sec * 1'000'000 + wall.tv_nsec / 1000;
  i64 base = (i64)monotonic.tv_sec * 1'000'000 + monotonic.tv_nsec / 1000;
  i64 at = base + (target - now);
  return at > 0 ? (u64)at : 0;
}

static int
TimeReady([[maybe_unused]] sd_event_source *source, [[maybe_unused]] u64 now, void *context)
{
  pa_time_event *event = context;
  struct timeval time;
  struct timespec wall;
  clock_gettime(CLOCK_REALTIME, &wall);
  time.tv_sec = wall.tv_sec;
  time.tv_usec = wall.tv_nsec / 1000;
  event->callback(&api, event, &time, event->context);
  return 0;
}

static void
TimeRestart(pa_time_event *event, const struct timeval *time)
{
  if (!time) {
    sd_event_source_set_enabled(event->source, SD_EVENT_OFF);
    return;
  }
  sd_event_source_set_time(event->source, Monotonic(time));
  sd_event_source_set_enabled(event->source, SD_EVENT_ONESHOT);
}

static pa_time_event *
TimeNew(
  [[maybe_unused]] pa_mainloop_api *self,
  const struct timeval *time,
  pa_time_event_cb_t callback,
  void *context
)
{
  pa_time_event *event = MemoryAllocateZeroed(1, sizeof(pa_time_event));
  event->callback = callback;
  event->context = context;
  sd_event_add_time(wayland.loop, &event->source, CLOCK_MONOTONIC, 0, 0, TimeReady, event);
  TimeRestart(event, time);
  return event;
}

static void
TimeFree(pa_time_event *event)
{
  sd_event_source_disable_unref(event->source);
  if (event->destroy) event->destroy(&api, event, event->context);
  free(event);
}

static void
TimeSetDestroy(pa_time_event *event, pa_time_event_destroy_cb_t destroy)
{
  event->destroy = destroy;
}

static int
DeferReady([[maybe_unused]] sd_event_source *source, void *context)
{
  pa_defer_event *event = context;
  event->callback(&api, event, event->context);
  return 0;
}

// a deferred event runs on every pass of the loop until it is disabled.
static pa_defer_event *
DeferNew([[maybe_unused]] pa_mainloop_api *self, pa_defer_event_cb_t callback, void *context)
{
  pa_defer_event *event = MemoryAllocateZeroed(1, sizeof(pa_defer_event));
  event->callback = callback;
  event->context = context;
  sd_event_add_defer(wayland.loop, &event->source, DeferReady, event);
  sd_event_source_set_enabled(event->source, SD_EVENT_ON);
  return event;
}

static void
DeferEnable(pa_defer_event *event, int enabled)
{
  sd_event_source_set_enabled(event->source, enabled ? SD_EVENT_ON : SD_EVENT_OFF);
}

static void
DeferFree(pa_defer_event *event)
{
  sd_event_source_disable_unref(event->source);
  if (event->destroy) event->destroy(&api, event, event->context);
  free(event);
}

static void
DeferSetDestroy(pa_defer_event *event, pa_defer_event_destroy_cb_t destroy)
{
  event->destroy = destroy;
}

static void
Quit([[maybe_unused]] pa_mainloop_api *self, [[maybe_unused]] int status)
{
}

pa_mainloop_api *
PulseApi(void)
{
  api = (pa_mainloop_api){
    .io_new = IoNew,
    .io_enable = IoEnable,
    .io_free = IoFree,
    .io_set_destroy = IoSetDestroy,
    .time_new = TimeNew,
    .time_restart = TimeRestart,
    .time_free = TimeFree,
    .time_set_destroy = TimeSetDestroy,
    .defer_new = DeferNew,
    .defer_enable = DeferEnable,
    .defer_free = DeferFree,
    .defer_set_destroy = DeferSetDestroy,
    .quit = Quit
  };
  return &api;
}
