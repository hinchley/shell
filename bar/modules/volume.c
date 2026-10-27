// volume: the default output's volume, from pulseaudio (pipewire-pulse).
// the wheel turns it up or down by 5%, a right click mutes or unmutes it, and a left click
// opens wiremix.

#include <linux/input-event-codes.h>
#include <math.h>
#include <pulse/context.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>
#include <pulse/volume.h>
#include <stdio.h>
#include <string.h>

#include "module.h"
#include "process.h"
#include "pulse.h"
#include "strbuf.h"

// how long to wait before connecting again after losing the server.
static constexpr u64 reconnectDelay = 2'000'000;
// a notch of the wheel, and how far it turns the volume.
static constexpr i32 notch = 120;
static constexpr f64 step = 0.05;

typedef struct Volume {
  pa_context *context;
  StrBuf sink;
  // the sink's channel count, and its volume (the channels' average) from 0 to 1.
  u8 channels;
  f64 level;
  bool muted;
  bool known;
  // wheel movement gathered towards a notch, positive upwards.
  i32 scroll;
  sd_event_source *retry;
} Volume;

static Volume volume;

static void
GotSink(
  [[maybe_unused]] pa_context *context,
  const pa_sink_info *info,
  int last,
  [[maybe_unused]] void *data
)
{
  if (last || !info) return;

  volume.channels = info->volume.channels;
  volume.level = (f64)pa_cvolume_avg(&info->volume) / PA_VOLUME_NORM;
  volume.muted = info->mute;
  volume.known = true;
  BarRedraw();
}

static void
GotServer(pa_context *context, const pa_server_info *info, [[maybe_unused]] void *data)
{
  if (!info || !info->default_sink_name) return;

  StrBufClear(&volume.sink);
  StrBufAppendString(&volume.sink, info->default_sink_name);
  pa_operation *operation =
    pa_context_get_sink_info_by_name(context, info->default_sink_name, GotSink, nullptr);
  if (operation) pa_operation_unref(operation);
}

// reads the default sink again: it, or which sink is the default, changed.
static void
Refresh(void)
{
  pa_operation *operation = pa_context_get_server_info(volume.context, GotServer, nullptr);
  if (operation) pa_operation_unref(operation);
}

static void
Event(
  [[maybe_unused]] pa_context *context,
  pa_subscription_event_type_t type,
  [[maybe_unused]] u32 index,
  [[maybe_unused]] void *data
)
{
  pa_subscription_event_type_t facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  if (facility == PA_SUBSCRIPTION_EVENT_SINK || facility == PA_SUBSCRIPTION_EVENT_SERVER) {
    Refresh();
  }
}

static void
Connect(void);

static int
Reconnect(
  [[maybe_unused]] sd_event_source *source,
  [[maybe_unused]] u64 now,
  [[maybe_unused]] void *context
)
{
  Connect();
  return 0;
}

static void
StateChanged(pa_context *context, [[maybe_unused]] void *data)
{
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY: {
      pa_context_set_subscribe_callback(context, Event, nullptr);
      pa_subscription_mask_t mask = PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER;
      pa_operation *operation = pa_context_subscribe(context, mask, nullptr, nullptr);
      if (operation) pa_operation_unref(operation);
      Refresh();
      break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      // the server went away: forget it, and connect again shortly.
      pa_context_unref(context);
      volume.context = nullptr;
      volume.known = false;
      BarRedraw();
      if (!volume.retry) {
        sd_event_add_time_relative(
          wayland.loop,
          &volume.retry,
          CLOCK_MONOTONIC,
          reconnectDelay,
          0,
          Reconnect,
          nullptr
        );
      } else {
        sd_event_source_set_time_relative(volume.retry, reconnectDelay);
        sd_event_source_set_enabled(volume.retry, SD_EVENT_ONESHOT);
      }
      break;
    default:
      break;
  }
}

static void
Connect(void)
{
  volume.context = pa_context_new(PulseApi(), "bar");
  pa_context_set_state_callback(volume.context, StateChanged, nullptr);
  pa_context_connect(volume.context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);
}

static void
Start(void)
{
  Connect();
}

static u32
Segments([[maybe_unused]] const BarOutput *output, Segment *segments)
{
  static char text[32];
  if (volume.muted) snprintf(text, sizeof(text), "\U000F0581 0%%");
  else snprintf(text, sizeof(text), "\U000F057E %d%%", (i32)round(volume.level * 100));
  segments[0] = SegmentLabel(text, themePurple);
  segments[0].action = 1;
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
  if (button == BTN_LEFT) {
    static const char *const program[] = {"wiremix", nullptr};
    ProcessTui("wiremix", program);
    return;
  }
  if (button != BTN_RIGHT || !volume.known || !volume.context) return;

  pa_operation *operation = pa_context_set_sink_mute_by_name(
    volume.context,
    volume.sink.data,
    !volume.muted,
    nullptr,
    nullptr
  );
  if (operation) pa_operation_unref(operation);
}

// each full notch of the wheel turns the volume 5% (and any part notch left over is dropped),
// as the quickshell bar did.
static void
Scroll([[maybe_unused]] BarOutput *output, [[maybe_unused]] u32 action, i32 value120)
{
  volume.scroll -= value120;
  if (volume.scroll > -notch && volume.scroll < notch) return;

  f64 level = volume.level + (volume.scroll > 0 ? step : -step);
  volume.scroll = 0;
  if (!volume.known || !volume.context) return;

  level = fmin(1, fmax(0, level));
  pa_cvolume levels;
  pa_cvolume_set(&levels, volume.channels, (pa_volume_t)round(level * PA_VOLUME_NORM));
  pa_operation *operation = pa_context_set_sink_volume_by_name(
    volume.context,
    volume.sink.data,
    &levels,
    nullptr,
    nullptr
  );
  if (operation) pa_operation_unref(operation);
  // the change is shown at once, rather than when the server confirms it.
  volume.level = level;
  BarRedraw();
}

const BarModule volumeModule = {
  .name = "volume",
  .Start = Start,
  .Segments = Segments,
  .Click = Click,
  .Scroll = Scroll,
  .padding = 30,
  .spacing = 4
};
